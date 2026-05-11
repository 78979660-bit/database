#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <iostream>
#include "PgConnection.h"

namespace Database
{
    namespace Network
    {

        using boost::asio::ip::tcp;

        class PgServer
        {
        public:
            PgServer(boost::asio::io_context &io_context, short port)
                : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
            {
                DoAccept();
            }

        private:
            void DoAccept()
            {
                acceptor_.async_accept(
                    [this](boost::system::error_code ec, tcp::socket socket)
                    {
                        if (!ec)
                        {
                            std::cout << "New connection accepted from " << socket.remote_endpoint() << std::endl;
                            std::make_shared<PgConnection>(std::move(socket))->Start();
                        }

                        DoAccept();
                    });
            }

            tcp::acceptor acceptor_;
        };

    } // namespace Network
} // namespace Database
