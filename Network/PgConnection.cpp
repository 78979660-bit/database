#include "PgConnection.h"
#include <iostream>

namespace Database
{
    namespace Network
    {

        PgConnection::PgConnection(tcp::socket socket)
            : socket_(std::move(socket)), buffer_(4096), startup_done_(false) {}

        void PgConnection::Start()
        {
            ReadStartupMessage();
        }

        void PgConnection::ReadStartupMessage()
        {
            auto self(shared_from_this());
            // In PostgreSQL, the startup message format starts with a 32-bit integer length
            socket_.async_read_some(boost::asio::buffer(buffer_),
                                    [this, self](boost::system::error_code ec, std::size_t length)
                                    {
                                        if (!ec)
                                        {
                                            // Here we should parse the startup packet: Length + Protocol Version + Params
                                            // Note: Protocol Version is 196608 (0x00030000) for Protocol v3.
                                            // For simplicity in this skeleton, we assume it's valid and accept the connection.

                                            std::cout << "Received Startup Message." << std::endl;

                                            SendAuthenticationOk();
                                            SendReadyForQuery();
                                            ReadCommand(); // Wait for actual SQL queries
                                        }
                                    });
        }

        void PgConnection::ReadCommand()
        {
            auto self(shared_from_this());
            socket_.async_read_some(boost::asio::buffer(buffer_),
                                    [this, self](boost::system::error_code ec, std::size_t length)
                                    {
                                        if (!ec)
                                        {
                                            if (length > 0)
                                            {
                                                char type = buffer_[0];
                                                if (type == 'Q')
                                                {
                                                    // Simple Query
                                                    std::cout << "Received Query Message." << std::endl;

                                                    // Extract query string (offset by 1 byte for type, 4 bytes for length)
                                                    if (length > 5)
                                                    {
                                                        std::string query(reinterpret_cast<char *>(&buffer_[5]), length - 6);
                                                        std::cout << "Query: " << query << std::endl;
                                                        // Here you'd route 'query' to your ExecutionEngine and Parser
                                                    }

                                                    // Send Dummy Complete & Ready
                                                    SendCommandComplete("SELECT 1");
                                                    SendReadyForQuery();
                                                }
                                                else if (type == 'P')
                                                {
                                                    std::cout << "Received Parse Message." << std::endl;
                                                    HandleParse(buffer_.data() + 1, length - 1);
                                                }
                                                else if (type == 'B')
                                                {
                                                    std::cout << "Received Bind Message." << std::endl;
                                                    HandleBind(buffer_.data() + 1, length - 1);
                                                }
                                                else if (type == 'D')
                                                {
                                                    std::cout << "Received Describe Message." << std::endl;
                                                    HandleDescribe(buffer_.data() + 1, length - 1);
                                                }
                                                else if (type == 'E')
                                                {
                                                    std::cout << "Received Execute Message." << std::endl;
                                                    HandleExecute(buffer_.data() + 1, length - 1);
                                                }
                                                else if (type == 'S')
                                                {
                                                    std::cout << "Received Sync Message." << std::endl;
                                                    HandleSync(buffer_.data() + 1, length - 1);
                                                }
                                                else if (type == 'X')
                                                {
                                                    // Terminate
                                                    std::cout << "Client disconnected." << std::endl;
                                                    return;
                                                }
                                                else
                                                {
                                                    std::cout << "Unhandled message type: " << type << std::endl;
                                                }
                                            }
                                            ReadCommand();
                                        }
                                    });
        }

        void PgConnection::HandleParse(const uint8_t *data, std::size_t length)
        {
            if (length < 4)
                return;
            // skipping 4 bytes length
            const char *str_data = reinterpret_cast<const char *>(data + 4);
            std::string stmt_name(str_data);
            std::string query(str_data + stmt_name.length() + 1);

            prepared_statements_[stmt_name] = {query};
            SendParseComplete();
        }

        void PgConnection::HandleBind(const uint8_t *data, std::size_t length)
        {
            if (length < 4)
                return;
            const char *str_data = reinterpret_cast<const char *>(data + 4);
            std::string portal_name(str_data);
            std::string stmt_name(str_data + portal_name.length() + 1);

            portals_[portal_name] = {stmt_name, {}};
            SendBindComplete();
        }

        void PgConnection::HandleDescribe(const uint8_t *data, std::size_t length)
        {
            // Usually returns ParameterDescription / RowDescription
            SendEmptyQueryResponse();
        }

        void PgConnection::HandleExecute(const uint8_t *data, std::size_t length)
        {
            // Execute the Portal
            SendCommandComplete("SELECT 1"); // Dummy logic
        }

        void PgConnection::HandleSync(const uint8_t *data, std::size_t length)
        {
            SendReadyForQuery();
        }

        void PgConnection::SendParseComplete()
        {
            std::vector<uint8_t> msg = {'1', 0, 0, 0, 4}; // ParseComplete identifier is '1'
            SendData(msg);
        }

        void PgConnection::SendBindComplete()
        {
            std::vector<uint8_t> msg = {'2', 0, 0, 0, 4}; // BindComplete identifier is '2'
            SendData(msg);
        }

        void PgConnection::SendEmptyQueryResponse()
        {
            std::vector<uint8_t> msg = {'n', 0, 0, 0, 4}; // NoData identifier is 'n'
            SendData(msg);
        }

        void PgConnection::SendAuthenticationOk()
        {
            // AuthOk message: 'R' + length(8) + AuthType(0 for OK)
            std::vector<uint8_t> msg = {'R', 0, 0, 0, 8, 0, 0, 0, 0};
            SendData(msg);
        }

        void PgConnection::SendReadyForQuery()
        {
            // ReadyForQuery message: 'Z' + length(5) + TransactionStatus('I' for idle)
            std::vector<uint8_t> msg = {'Z', 0, 0, 0, 5, 'I'};
            SendData(msg);
        }

        void PgConnection::SendCommandComplete(const std::string &tag)
        {
            std::vector<uint8_t> msg;
            msg.push_back('C');
            uint32_t len = 4 + tag.length() + 1; // 4 for length itself + length of string + null terminator

            // PG uses network byte order (Big Endian)
            msg.push_back((len >> 24) & 0xFF);
            msg.push_back((len >> 16) & 0xFF);
            msg.push_back((len >> 8) & 0xFF);
            msg.push_back(len & 0xFF);

            for (char c : tag)
            {
                msg.push_back(static_cast<uint8_t>(c));
            }
            msg.push_back(0); // null terminator

            SendData(msg);
        }

        void PgConnection::SendData(const std::vector<uint8_t> &data)
        {
            auto self(shared_from_this());
            boost::asio::async_write(socket_, boost::asio::buffer(data),
                                     [this, self](boost::system::error_code ec, std::size_t /*length*/)
                                     {
                                         if (ec)
                                         {
                                             std::cerr << "Write error: " << ec.message() << std::endl;
                                         }
                                     });
        }

    } // namespace Network
} // namespace Database
