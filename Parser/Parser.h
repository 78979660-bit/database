#pragma once

#include "Lexer.h"
#include "SQLStatement.h"
#include <vector>
#include <memory>
#include <stdexcept>

namespace Database
{

    class Parser
    {
    public:
        Parser(std::vector<Token> tokens);

        std::unique_ptr<SQLStatement> Parse();

    private:
        Token Peek() const;
        Token Advance();
        Token Consume(TokenType expected_type, const std::string &error_message);
        bool Match(TokenType type);

        std::unique_ptr<CreateStatement> ParseCreateTable();
        std::unique_ptr<InsertStatement> ParseInsert();
        std::unique_ptr<SelectStatement> ParseSelect();
        std::vector<CommonTableExpression> ParseWithClause();

        std::vector<Token> tokens_;
        size_t pos_ = 0;
    };

} // namespace Database