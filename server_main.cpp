#include <iostream>
#include <boost/asio.hpp>
#include "Network/Server.h"

int main()
{
    try
    {
        boost::asio::io_context io_context;
        // PostgreSQL default port
        short port = 5432;

        std::cout << "Starting PostgreSQL compatible server on port " << port << "..." << std::endl;
        Database::Network::PgServer server(io_context, port);

        // This will block and run the event loop
        io_context.run();
    }
    catch (std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
