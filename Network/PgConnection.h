#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace Database
{
    namespace Network
    {

        using boost::asio::ip::tcp;

        struct PreparedStatement
        {
            std::string query;
        };

        struct Portal
        {
            std::string statement_name;
            std::vector<std::vector<uint8_t>> parameters;
        };

        class PgConnection : public std::enable_shared_from_this<PgConnection>
        {
        public:
            PgConnection(tcp::socket socket);
            void Start();

        private:
            void ReadStartupMessage();
            void ReadCommand();

            void HandleParse(const uint8_t *data, std::size_t length);
            void HandleBind(const uint8_t *data, std::size_t length);
            void HandleExecute(const uint8_t *data, std::size_t length);
            void HandleDescribe(const uint8_t *data, std::size_t length);
            void HandleSync(const uint8_t *data, std::size_t length);

            void SendAuthenticationOk();
            void SendReadyForQuery();
            void SendCommandComplete(const std::string &tag);
            void SendErrorResponse(const std::string &message);
            void SendParseComplete();
            void SendBindComplete();
            void SendEmptyQueryResponse();
            void SendData(const std::vector<uint8_t> &data);

            tcp::socket socket_;
            std::vector<uint8_t> buffer_;
            bool startup_done_;

            std::unordered_map<std::string, PreparedStatement> prepared_statements_;
            std::unordered_map<std::string, Portal> portals_;
        };

    } // namespace Network
} // namespace Database
