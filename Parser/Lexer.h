#pragma once

#include <string>
#include <vector>

#undef IN

namespace Database
{

    enum class TokenType
    {
        INVALID = 0,
        IDENTIFIER,
        NUMBER,
        STRING_LITERAL, // e.g. 'hello'

        // Keywords
        CREATE,
        TABLE,
        INSERT,
        INTO,
        VALUES,
        SELECT,
        FROM,
        WHERE,
        INT,
        VARCHAR,
        GROUP,
        BY,
        COUNT,
        SUM,
        MIN,
        MAX,
        AVG,
        HAVING,
        IN,
        AS,
        WITH,
        OVER,
        PARTITION,
        ORDER,
        RANK,
        ROW_NUMBER,
        ASC,
        DESC,

        // Symbols
        LPAREN,    // (
        RPAREN,    // )
        COMMA,     // ,
        SEMICOLON, // ;
        ASTERISK,  // *
        EQUALS,    // =

        END_OF_FILE
    };

    struct Token
    {
        TokenType type_;
        std::string value_;
        int line_;
        int column_;

        Token(TokenType type, std::string value, int line, int column)
            : type_(type), value_(std::move(value)), line_(line), column_(column) {}
    };

    class Lexer
    {
    public:
        Lexer(const std::string &input);

        std::vector<Token> Tokenize();

    private:
        char Peek() const;
        char Advance();
        void SkipWhitespace();
        Token NextToken();

        TokenType CheckKeyword(const std::string &str);

        std::string input_;
        size_t pos_ = 0;
        int line_ = 1;
        int column_ = 1;
    };

} // namespace Database