///////////////////////////////////////////////////////////////////////////////
//
// CoinQ_peer_io.cpp
//
// Copyright (c) 2012-2013 Eric Lombrozo
//
// All Rights Reserved.

#include "CoinQ_peer_io.h"

using namespace CoinQ;

const unsigned char Peer::DEFAULT_Ipv6[] = {0,0,0,0,0,0,0,0,0,0,255,255,127,0,0,1};

void Peer::do_handshake()
{
    if (!bRunning) return;

    Coin::NetworkAddress peerAddress;
    peerAddress.set(NODE_NETWORK, DEFAULT_Ipv6, strtoul(port_.c_str(), NULL, 0));
    Coin::VersionMessage versionMessage(protocol_version_, NODE_NETWORK, time(NULL), peerAddress, peerAddress, getRandomNonce64(), user_agent_.c_str(), start_height_, relay_);
    Coin::CoinNodeMessage msg(magic_bytes_, &versionMessage);
    do_send(msg);

    timer_.expires_from_now(boost::posix_time::seconds(5));
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (bHandshakeComplete) return;
        boost::lock_guard<boost::mutex> lock(handshakeMutex);
        if (bHandshakeComplete) return;
        stop();
        notifyTimeout(*this);
    });
}

void Peer::do_read()
{
    boost::asio::async_read(socket_, boost::asio::buffer(read_buffer, READ_BUFFER_SIZE), boost::asio::transfer_at_least(MIN_MESSAGE_HEADER_SIZE),
    strand_.wrap([this](const boost::system::error_code& ec, std::size_t bytes_read) {
        //std::cout << "error code: " << ec.value() << ": " << ec.message() << std::endl;
        //std::cout << "bytes read: " << bytes_read << std::endl;
        if (ec) {
            stop();
            notifyClose(*this, ec.value(), ec.message());
            return;
        }

        read_message += uchar_vector(read_buffer, bytes_read);

        while (read_message.size() >= MIN_MESSAGE_HEADER_SIZE) {
            // Find the first occurrence of the magic bytes, discard anything before it.
            // If magic bytes are not found, read new buffer. (TODO: detect misbehaving node and disconnect.)
            uchar_vector::iterator it = std::search(read_message.begin(), read_message.end(), magic_bytes_vector_.begin(), magic_bytes_vector_.end());
            if (it == read_message.end()) {
                read_message.clear();
                break;
            }
            read_message.assign(it, read_message.end());
            if (read_message.size() < MIN_MESSAGE_HEADER_SIZE) {
                break;
            }

            // Get command
            unsigned char command[13];
            command[12] = 0;
            uchar_vector(read_message.begin() + 4, read_message.begin() + 16).copyToArray(command);

            // Get payload size
            unsigned int payloadSize = vch_to_uint<uint32_t>(uchar_vector(read_message.begin() + 16, read_message.begin() + 20), _BIG_ENDIAN);

            if (read_message.size() < MIN_MESSAGE_HEADER_SIZE  + payloadSize) {
                break;
            }

           try {
                Coin::CoinNodeMessage peerMessage(read_message);
                //std::cout << "Data read: " << peerMessage.getSerialized().getHex() << std::endl;
                //std::cout << "Received message: " << peerMessage.toIndentedString() << std::endl;

                if (!peerMessage.isChecksumValid()) {
                    throw std::runtime_error("Invalid checksum.");
                }

                std::string command = peerMessage.getCommand();
                if (command == "verack") {
                    // Signal completion of handshake
                    if (bHandshakeComplete) {
                        throw std::runtime_error("Second verack received.");
                    }
                    boost::unique_lock<boost::mutex> lock(handshakeMutex);
                    if (bHandshakeComplete) {
                        throw std::runtime_error("Second verack received.");
                    }
                    timer_.cancel();
                    bHandshakeComplete = true;
                    lock.unlock();
                    bWriteReady = true;
                    notifyOpen(*this);
                }
                else if (command == "version") {
                    // TODO: Check version information
                    Coin::VerackMessage verackMessage;
                    Coin::CoinNodeMessage msg(magic_bytes_, &verackMessage);
                    do_send(msg);
                }
                else if (command == "inv") {
                    Coin::Inventory* pInventory = static_cast<Coin::Inventory*>(peerMessage.getPayload());
                    notifyInv(*this, *pInventory);
                }
                else if (command == "tx") {
                    Coin::Transaction* pTx = static_cast<Coin::Transaction*>(peerMessage.getPayload());
                    notifyTx(*this, *pTx);
                }
                else if (command == "block") {
                    Coin::CoinBlock* pBlock = static_cast<Coin::CoinBlock*>(peerMessage.getPayload());
                    notifyBlock(*this, *pBlock);
                }
                else if (command == "merkleblock") {
                    Coin::MerkleBlock* pMerkleBlock = static_cast<Coin::MerkleBlock*>(peerMessage.getPayload());
                    notifyMerkleBlock(*this, *pMerkleBlock);
                }
                else if (command == "addr") {
                    Coin::AddrMessage* pAddr = static_cast<Coin::AddrMessage*>(peerMessage.getPayload());
                    notifyAddr(*this, *pAddr);
                }
                else if (command == "headers") {
                    Coin::HeadersMessage* pHeaders = static_cast<Coin::HeadersMessage*>(peerMessage.getPayload());
                    notifyHeaders(*this, *pHeaders);
                }
                else {
                    std::cout << "Command type not implemented: " << command << std::endl;
                }
                notifyMessage(*this, peerMessage);
            }
            catch (const std::exception& e) {
                std::cout << "Message exception: " << e.what() << std::endl;
            }

            read_message.assign(read_message.begin() + MIN_MESSAGE_HEADER_SIZE + payloadSize, read_message.end());
        }

        do_read();
    }));
}

void Peer::do_write(boost::shared_ptr<uchar_vector> data)
{
    //std::cout << "Data to write: " << data.getHex() << std::endl;
    boost::asio::async_write(socket_, boost::asio::buffer(*data), boost::asio::transfer_all(),
    strand_.wrap([this](const boost::system::error_code& ec, std::size_t bytes_written) {
        if (ec) {
            std::cout << "Peer::send() - Error " << ec.value() << ": " << ec.message() << std::endl;
            return;
        }
        boost::lock_guard<boost::mutex> sendLock(sendMutex);
        sendQueue.pop();
        if (!sendQueue.empty()) {
            do_write(sendQueue.front());
        }
    }));
}

void Peer::do_send(const Coin::CoinNodeMessage& message)
{
    boost::shared_ptr<uchar_vector> data(new uchar_vector(message.getSerialized()));
    boost::lock_guard<boost::mutex> sendLock(sendMutex);
    sendQueue.push(data);
    if (sendQueue.size() == 1) {
        strand_.post(boost::bind(&Peer::do_write, this, data));
    }
}

void Peer::do_connect(tcp::resolver::iterator iter)
{
    boost::asio::async_connect(socket_, iter, strand_.wrap([&](const boost::system::error_code& ec, tcp::resolver::iterator) {
        if (ec) {
            std::cout << "Peer::start() - Connection error: " << ec.message() << "." << std::endl;
            stop();
            notifyClose(*this, ec.value(), ec.message());
        }
        else {
            try {
                do_read();
                do_handshake();
            }
            catch (const boost::system::error_code& ec) {
                std::cout << "Connection handler error " << ec.value() << ": " << ec.message() << std::endl;
                bRunning = false;
            }
            catch (const std::exception& e) {
                std::cout << "Connection handler std::exception - " << e.what() << std::endl;
            }
        }
    }));
}



void Peer::start()

{
    if (bRunning) {
        throw std::runtime_error("Peer already started.");
    }

    boost::unique_lock<boost::shared_mutex> lock(mutex);
    if (bRunning) {
        throw std::runtime_error("Peer already started.");
    }

    tcp::resolver::query query(host_, port_);

    resolver_.async_resolve(query, [this](const boost::system::error_code& ec, tcp::resolver::iterator iterator) {
        if (ec) {
            std::cout << "Peer::start() - Error " << ec.value() << ": " << ec.message() << std::endl;
            notifyClose(*this, ec.value(), ec.message());
            return;
        }
        endpoint_ = *iterator;
        bWriteReady = false;
        bHandshakeComplete = false;
        bRunning = true;
        strand_.post(boost::bind(&Peer::do_connect, this, iterator)); 
    }); 
}

void Peer::stop()
{
    if (!bRunning) return;
    boost::unique_lock<boost::shared_mutex> lock(mutex);
    if (!bRunning) return;

    socket_.cancel();
    socket_.close();
    bRunning = false;
    bHandshakeComplete = false;
    bWriteReady = false;
    notifyStop(*this);
}

bool Peer::send(Coin::CoinNodeStructure& message)
{
    boost::shared_lock<boost::shared_mutex> runLock(mutex);
    if (!bRunning || !bWriteReady) return false;

    Coin::CoinNodeMessage wrappedMessage(magic_bytes_, &message);
    do_send(wrappedMessage);
    return true;
}

