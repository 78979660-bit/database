#include "Lexer.h"
#include <cctype>
#include <algorithm>

namespace Database
{

    Lexer::Lexer(const std::string &input) : input_(input) {}

    std::vector<Token> Lexer::Tokenize()
    {
        std::vector<Token> tokens;
        Token tok = NextToken();
        while (tok.type_ != TokenType::END_OF_FILE)
        {
            tokens.push_back(tok);
            tok = NextToken();
        }
        tokens.push_back(tok); // add END_OF_FILE
        return tokens;
    }

    char Lexer::Peek() const
    {
        if (pos_ >= input_.length())
            return '\0';
        return input_[pos_];
    }

    char Lexer::Advance()
    {
        if (pos_ >= input_.length())
            return '\0';
        char c = input_[pos_++];
        if (c == '\n')
        {
            line_++;
            column_ = 1;
        }
        else
        {
            column_++;
        }
        return c;
    }

    void Lexer::SkipWhitespace()
    {
        while (std::isspace(Peek()))
        {
            Advance();
        }
    }

    TokenType Lexer::CheckKeyword(const std::string &str)
    {
        std::string upper_str = str;
        std::transform(upper_str.begin(), upper_str.end(), upper_str.begin(), ::toupper);

        if (upper_str == "CREATE")
            return TokenType::CREATE;
        if (upper_str == "TABLE")
            return TokenType::TABLE;
        if (upper_str == "INSERT")
            return TokenType::INSERT;
        if (upper_str == "INTO")
            return TokenType::INTO;
        if (upper_str == "VALUES")
            return TokenType::VALUES;
        if (upper_str == "SELECT")
            return TokenType::SELECT;
        if (upper_str == "FROM")
            return TokenType::FROM;
        if (upper_str == "WHERE")
            return TokenType::WHERE;
        if (upper_str == "GROUP")
            return TokenType::GROUP;
        if (upper_str == "BY")
            return TokenType::BY;
        if (upper_str == "COUNT")
            return TokenType::COUNT;
        if (upper_str == "SUM")
            return TokenType::SUM;
        if (upper_str == "MIN")
            return TokenType::MIN;
        if (upper_str == "MAX")
            return TokenType::MAX;
        if (upper_str == "AVG")
            return TokenType::AVG;
        if (upper_str == "HAVING")
            return TokenType::HAVING;
        if (upper_str == "IN")
            return TokenType::IN;
        if (upper_str == "AS")
            return TokenType::AS;
        if (upper_str == "WITH")
            return TokenType::WITH;
        if (upper_str == "OVER")
            return TokenType::OVER;
        if (upper_str == "PARTITION")
            return TokenType::PARTITION;
        if (upper_str == "ORDER")
            return TokenType::ORDER;
        if (upper_str == "RANK")
            return TokenType::RANK;
        if (upper_str == "ROW_NUMBER")
            return TokenType::ROW_NUMBER;
        if (upper_str == "ASC")
            return TokenType::ASC;
        if (upper_str == "DESC")
            return TokenType::DESC;
        if (upper_str == "INT" || upper_str == "INTEGER")
            return TokenType::INT;
        if (upper_str == "VARCHAR")
            return TokenType::VARCHAR;

        return TokenType::IDENTIFIER;
    }

    Token Lexer::NextToken()
    {
        SkipWhitespace();

        char c = Peek();
        if (c == '\0')
        {
            return Token(TokenType::END_OF_FILE, "", line_, column_);
        }

        int start_col = column_;

        // Symbols
        if (c == '(')
        {
            Advance();
            return Token(TokenType::LPAREN, "(", line_, start_col);
        }
        if (c == ')')
        {
            Advance();
            return Token(TokenType::RPAREN, ")", line_, start_col);
        }
        if (c == ',')
        {
            Advance();
            return Token(TokenType::COMMA, ",", line_, start_col);
        }
        if (c == ';')
        {
            Advance();
            return Token(TokenType::SEMICOLON, ";", line_, start_col);
        }
        if (c == '*')
        {
            Advance();
            return Token(TokenType::ASTERISK, "*", line_, start_col);
        }
        if (c == '=')
        {
            Advance();
            return Token(TokenType::EQUALS, "=", line_, start_col);
        }

        // Numbers
        if (std::isdigit(c))
        {
            std::string num;
            while (std::isdigit(Peek()))
            {
                num += Advance();
            }
            return Token(TokenType::NUMBER, num, line_, start_col);
        }

        // String literals '...'
        if (c == '\'')
        {
            Advance(); // skip '
            std::string str;
            while (Peek() != '\'' && Peek() != '\0')
            {
                str += Advance();
            }
            if (Peek() == '\'')
                Advance(); // skip '
            return Token(TokenType::STRING_LITERAL, str, line_, start_col);
        }

        // Identifiers and Keywords
        if (std::isalpha(c) || c == '_')
        {
            std::string id;
            while (std::isalnum(Peek()) || Peek() == '_')
            {
                id += Advance();
            }
            TokenType type = CheckKeyword(id);
            return Token(type, id, line_, start_col);
        }

        // Invalid character
        std::string invalid(1, Advance());
        return Token(TokenType::INVALID, invalid, line_, start_col);
    }

} // namespace Database