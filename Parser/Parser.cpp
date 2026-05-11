#include "Parser.h"
#include <iostream>
#include "../Execution/Expressions/ColumnValueExpression.h"
#include "../Execution/Expressions/ConstantValueExpression.h"
#include "../Execution/Expressions/ComparisonExpression.h"
#include "../Execution/Expressions/InSubqueryExpression.h"

namespace Database
{

    Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Token Parser::Peek() const
    {
        if (pos_ >= tokens_.size())
            return tokens_.back(); // Return EOF if at end
        return tokens_[pos_];
    }

    Token Parser::Advance()
    {
        if (pos_ >= tokens_.size())
            return tokens_.back();
        return tokens_[pos_++];
    }

    Token Parser::Consume(TokenType expected_type, const std::string &error_message)
    {
        if (Peek().type_ == expected_type)
        {
            return Advance();
        }
        throw std::runtime_error("Parser Error at line " + std::to_string(Peek().line_) +
                                 ", col " + std::to_string(Peek().column_) + ": " +
                                 error_message + ". Got " + Peek().value_);
    }

    bool Parser::Match(TokenType type)
    {
        if (Peek().type_ == type)
        {
            Advance();
            return true;
        }
        return false;
    }

    std::unique_ptr<SQLStatement> Parser::Parse()
    {
        std::vector<CommonTableExpression> ctes;
        if (Peek().type_ == TokenType::WITH)
        {
            ctes = ParseWithClause();
        }

        if (Match(TokenType::CREATE))
        {
            if (!ctes.empty())
                throw std::runtime_error("Parser Error: WITH clause not supported before CREATE");
            return ParseCreateTable();
        }
        else if (Match(TokenType::INSERT))
        {
            if (!ctes.empty())
                throw std::runtime_error("Parser Error: WITH clause not supported before INSERT");
            return ParseInsert();
        }
        else if (Match(TokenType::SELECT))
        {
            auto select_stmt = ParseSelect();
            select_stmt->ctes_ = std::move(ctes);
            return select_stmt;
        }
        throw std::runtime_error("Parser Error: Unknown statement type starting with " + Peek().value_);
    }

    std::vector<CommonTableExpression> Parser::ParseWithClause()
    {
        Consume(TokenType::WITH, "Expected WITH keyword");
        std::vector<CommonTableExpression> ctes;

        do
        {
            Token cte_name = Consume(TokenType::IDENTIFIER, "Expected CTE name");
            Consume(TokenType::AS, "Expected AS after CTE name");
            Consume(TokenType::LPAREN, "Expected '(' after AS in CTE");

            Consume(TokenType::SELECT, "Expected SELECT in CTE body");
            auto cte_query = ParseSelect();
            // Note: ParseSelect consumes the optional semicolon. Let's not consume it recursively for subqueries ideally, but for now we continue since match(SEMICOLON) in ParseSelect is optional.

            Consume(TokenType::RPAREN, "Expected ')' after CTE query");

            ctes.push_back({cte_name.value_, std::move(cte_query)});
        } while (Match(TokenType::COMMA));

        return ctes;
    }

    // CREATE TABLE table_name (col1 type1, col2 type2, ...)
    std::unique_ptr<CreateStatement> Parser::ParseCreateTable()
    {
        Consume(TokenType::TABLE, "Expected TABLE after CREATE");
        Token table_name_tok = Consume(TokenType::IDENTIFIER, "Expected table name");

        Consume(TokenType::LPAREN, "Expected '(' after table name");

        std::vector<Column> columns;
        do
        {
            Token col_name_tok = Consume(TokenType::IDENTIFIER, "Expected column name");
            Token type_tok = Advance();

            TypeId type;
            if (type_tok.type_ == TokenType::INT)
            {
                type = TypeId::INTEGER;
            }
            else if (type_tok.type_ == TokenType::VARCHAR)
            {
                type = TypeId::VARCHAR;
            }
            else
            {
                throw std::runtime_error("Parser Error: Unknown type " + type_tok.value_);
            }

            columns.emplace_back(col_name_tok.value_, type);

        } while (Match(TokenType::COMMA));

        Consume(TokenType::RPAREN, "Expected ')' after column definitions");
        Match(TokenType::SEMICOLON); // Optional semicolon

        return std::make_unique<CreateStatement>(table_name_tok.value_, columns);
    }

    // INSERT INTO table_name VALUES (val1, val2), (...)
    std::unique_ptr<InsertStatement> Parser::ParseInsert()
    {
        Consume(TokenType::INTO, "Expected INTO after INSERT");
        Token table_name_tok = Consume(TokenType::IDENTIFIER, "Expected table name");

        Consume(TokenType::VALUES, "Expected VALUES");

        std::vector<std::vector<Value>> all_values;
        do
        {
            Consume(TokenType::LPAREN, "Expected '(' before values");
            std::vector<Value> row_values;
            do
            {
                Token val_tok = Advance();
                if (val_tok.type_ == TokenType::NUMBER)
                {
                    row_values.emplace_back(std::stoi(val_tok.value_));
                }
                else if (val_tok.type_ == TokenType::STRING_LITERAL)
                {
                    row_values.emplace_back(val_tok.value_);
                }
                else
                {
                    throw std::runtime_error("Parser Error: Expected number or string literal");
                }
            } while (Match(TokenType::COMMA));

            Consume(TokenType::RPAREN, "Expected ')' after values");
            all_values.push_back(row_values);
        } while (Match(TokenType::COMMA));

        Match(TokenType::SEMICOLON); // Optional semicolon

        return std::make_unique<InsertStatement>(table_name_tok.value_, all_values);
    }

    // SELECT col1, col2, COUNT(*), ... FROM table_name
    std::unique_ptr<SelectStatement> Parser::ParseSelect()
    {
        std::vector<std::string> select_list;
        bool has_count_star = false;
        std::vector<ParsedAggregation> aggregations;
        std::vector<ParsedWindowFunction> window_functions;

        if (Match(TokenType::ASTERISK))
        {
            select_list.push_back("*");
        }
        else
        {
            do
            {
                TokenType type = Peek().type_;
                if (type == TokenType::COUNT)
                {
                    Advance();
                    Consume(TokenType::LPAREN, "Expected '(' after COUNT");
                    std::string func_colName = "";
                    if (Match(TokenType::ASTERISK))
                    {
                        Consume(TokenType::RPAREN, "Expected ')' after COUNT(*)");
                        has_count_star = true;
                        func_colName = "*";
                    }
                    else
                    {
                        Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name");
                        Consume(TokenType::RPAREN, "Expected ')'");
                        aggregations.push_back({"COUNT", col_tok.value_});
                        func_colName = col_tok.value_;
                    }

                    if (Match(TokenType::OVER))
                    {
                        Consume(TokenType::LPAREN, "Expected '(' after OVER");
                        std::vector<std::string> partition_by;
                        std::vector<std::pair<std::string, bool>> order_by;

                        if (Match(TokenType::PARTITION))
                        {
                            Consume(TokenType::BY, "Expected BY after PARTITION");
                            do
                            {
                                Token part_col = Consume(TokenType::IDENTIFIER, "Expected column name in PARTITION BY");
                                partition_by.push_back(part_col.value_);
                            } while (Match(TokenType::COMMA));
                        }
                        if (Match(TokenType::ORDER))
                        {
                            Consume(TokenType::BY, "Expected BY after ORDER");
                            do
                            {
                                Token order_col = Consume(TokenType::IDENTIFIER, "Expected column name in ORDER BY");
                                bool is_asc = true;
                                if (Match(TokenType::DESC)) is_asc = false;
                                else Match(TokenType::ASC);
                                order_by.push_back({order_col.value_, is_asc});
                            } while (Match(TokenType::COMMA));
                        }
                        Consume(TokenType::RPAREN, "Expected ')' after OVER clause");
                        window_functions.push_back({"COUNT", func_colName, partition_by, order_by});
                    }
                }
                else if (type == TokenType::SUM || type == TokenType::MIN || type == TokenType::MAX || type == TokenType::AVG)
                {
                    Token func_tok = Advance();
                    Consume(TokenType::LPAREN, "Expected '('");
                    Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name");
                    Consume(TokenType::RPAREN, "Expected ')'");
                    std::string func_name = "";
                    if (type == TokenType::SUM)
                        func_name = "SUM";
                    else if (type == TokenType::MIN)
                        func_name = "MIN";
                    else if (type == TokenType::MAX)
                        func_name = "MAX";
                    else if (type == TokenType::AVG)
                        func_name = "AVG";

                    aggregations.push_back({func_name, col_tok.value_});

                    if (Match(TokenType::OVER))
                    {
                        Consume(TokenType::LPAREN, "Expected '(' after OVER");
                        std::vector<std::string> partition_by;
                        std::vector<std::pair<std::string, bool>> order_by;

                        if (Match(TokenType::PARTITION))
                        {
                            Consume(TokenType::BY, "Expected BY after PARTITION");
                            do
                            {
                                Token part_col = Consume(TokenType::IDENTIFIER, "Expected column name in PARTITION BY");
                                partition_by.push_back(part_col.value_);
                            } while (Match(TokenType::COMMA));
                        }
                        if (Match(TokenType::ORDER))
                        {
                            Consume(TokenType::BY, "Expected BY after ORDER");
                            do
                            {
                                Token order_col = Consume(TokenType::IDENTIFIER, "Expected column name in ORDER BY");
                                bool is_asc = true;
                                if (Match(TokenType::DESC)) is_asc = false;
                                else Match(TokenType::ASC);
                                order_by.push_back({order_col.value_, is_asc});
                            } while (Match(TokenType::COMMA));
                        }
                        Consume(TokenType::RPAREN, "Expected ')' after OVER clause");
                        window_functions.push_back({func_name, col_tok.value_, partition_by, order_by});
                    }
                }
                else if (type == TokenType::RANK || type == TokenType::ROW_NUMBER)
                {
                    Token func_tok = Advance();
                    Consume(TokenType::LPAREN, "Expected '('");
                    Consume(TokenType::RPAREN, "Expected ')'");
                    std::string func_name = (type == TokenType::RANK) ? "RANK" : "ROW_NUMBER";

                    if (Match(TokenType::OVER))
                    {
                        Consume(TokenType::LPAREN, "Expected '(' after OVER");
                        std::vector<std::string> partition_by;
                        std::vector<std::pair<std::string, bool>> order_by;

                        if (Match(TokenType::PARTITION))
                        {
                            Consume(TokenType::BY, "Expected BY after PARTITION");
                            do
                            {
                                Token part_col = Consume(TokenType::IDENTIFIER, "Expected column name in PARTITION BY");
                                partition_by.push_back(part_col.value_);
                            } while (Match(TokenType::COMMA));
                        }
                        if (Match(TokenType::ORDER))
                        {
                            Consume(TokenType::BY, "Expected BY after ORDER");
                            do
                            {
                                Token order_col = Consume(TokenType::IDENTIFIER, "Expected column name in ORDER BY");
                                bool is_asc = true;
                                if (Match(TokenType::DESC)) is_asc = false;
                                else Match(TokenType::ASC);
                                order_by.push_back({order_col.value_, is_asc});
                            } while (Match(TokenType::COMMA));
                        }
                        Consume(TokenType::RPAREN, "Expected ')' after OVER clause");
                        window_functions.push_back({func_name, "", partition_by, order_by});
                    }
                    else
                    {
                        throw std::runtime_error("Expected OVER clause for window function " + func_name);
                    }
                }
                else
                {
                    Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name");
                    select_list.push_back(col_tok.value_);
                }
            } while (Match(TokenType::COMMA));
        }

        Consume(TokenType::FROM, "Expected FROM");
        std::string table_name_str = "";
        std::shared_ptr<SelectStatement> subquery = nullptr;
        std::string table_alias = "";

        if (Match(TokenType::LPAREN))
        {
            Consume(TokenType::SELECT, "Expected SELECT inside FROM subquery");
            subquery = ParseSelect();
            Consume(TokenType::RPAREN, "Expected ')' after subquery");

            if (Match(TokenType::AS))
            {
                table_alias = Consume(TokenType::IDENTIFIER, "Expected alias after AS").value_;
            }
            else if (Peek().type_ == TokenType::IDENTIFIER)
            {
                table_alias = Advance().value_;
            }
        }
        else
        {
            Token table_name_tok = Consume(TokenType::IDENTIFIER, "Expected table name");
            table_name_str = table_name_tok.value_;
            if (Match(TokenType::AS))
            {
                table_alias = Consume(TokenType::IDENTIFIER, "Expected alias after AS").value_;
            }
            else if (Peek().type_ == TokenType::IDENTIFIER)
            {
                table_alias = Advance().value_;
            }
        }

        std::shared_ptr<AbstractExpression> where_filter = nullptr;
        if (Match(TokenType::WHERE))
        {
            Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name in WHERE clause");
            auto col_expr = std::make_shared<ColumnValueExpression>(0, col_tok.value_);

            if (Match(TokenType::EQUALS))
            {
                Token val_tok = Advance();
                Value val;
                if (val_tok.type_ == TokenType::NUMBER)
                {
                    val = Value(std::stoi(val_tok.value_));
                }
                else if (val_tok.type_ == TokenType::STRING_LITERAL)
                {
                    val = Value(val_tok.value_);
                }
                else
                {
                    throw std::runtime_error("Parser Error: Expected number or string literal in WHERE clause");
                }
                auto const_expr = std::make_shared<ConstantValueExpression>(val);
                where_filter = std::make_shared<ComparisonExpression>(col_expr, const_expr, CompType::Equal);
            }
            else if (Match(TokenType::IN))
            {
                Consume(TokenType::LPAREN, "Expected '(' after IN");
                Consume(TokenType::SELECT, "Expected SELECT inside IN subquery");
                auto in_subquery = ParseSelect();
                Consume(TokenType::RPAREN, "Expected ')' after IN subquery");
                where_filter = std::make_shared<InSubqueryExpression>(col_expr, std::move(in_subquery));
            }
            else
            {
                throw std::runtime_error("Parser Error: Expected '=' or 'IN' in WHERE clause");
            }
        }

        std::vector<std::string> group_by_list;
        if (Match(TokenType::GROUP))
        {
            Consume(TokenType::BY, "Expected BY after GROUP");
            do
            {
                Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name in GROUP BY");
                group_by_list.push_back(col_tok.value_);
            } while (Match(TokenType::COMMA));
        }

        std::shared_ptr<AbstractExpression> having_filter = nullptr;
        if (Match(TokenType::HAVING))
        {
            std::string left_col_name;
            TokenType type = Peek().type_;
            if (type == TokenType::COUNT)
            {
                Advance();
                Consume(TokenType::LPAREN, "Expected '(' after COUNT");
                if (Match(TokenType::ASTERISK))
                {
                    Consume(TokenType::RPAREN, "Expected ')' after COUNT(*)");
                    left_col_name = "COUNT(*)";
                }
                else
                {
                    Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name inside COUNT");
                    Consume(TokenType::RPAREN, "Expected ')'");
                    left_col_name = "COUNT(" + col_tok.value_ + ")";
                }
            }
            else if (type == TokenType::SUM || type == TokenType::MIN || type == TokenType::MAX || type == TokenType::AVG)
            {
                Token func_tok = Advance();
                Consume(TokenType::LPAREN, "Expected '('");
                Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name");
                Consume(TokenType::RPAREN, "Expected ')'");
                std::string func_name = "";
                if (type == TokenType::SUM)
                    func_name = "SUM";
                else if (type == TokenType::MIN)
                    func_name = "MIN";
                else if (type == TokenType::MAX)
                    func_name = "MAX";
                else if (type == TokenType::AVG)
                    func_name = "AVG";
                left_col_name = func_name + "(" + col_tok.value_ + ")";
            }
            else
            {
                Token col_tok = Consume(TokenType::IDENTIFIER, "Expected column name or aggregate in HAVING clause");
                left_col_name = col_tok.value_;
            }

            Consume(TokenType::EQUALS, "Expected '=' in HAVING clause");
            Token val_tok = Advance();
            Value val;
            if (val_tok.type_ == TokenType::NUMBER)
            {
                val = Value(std::stoi(val_tok.value_));
            }
            else if (val_tok.type_ == TokenType::STRING_LITERAL)
            {
                val = Value(val_tok.value_);
            }
            else
            {
                throw std::runtime_error("Parser Error: Expected number or string literal in HAVING clause");
            }

            auto col_expr = std::make_shared<ColumnValueExpression>(0, left_col_name);
            auto const_expr = std::make_shared<ConstantValueExpression>(val);
            having_filter = std::make_shared<ComparisonExpression>(col_expr, const_expr, CompType::Equal);
        }

        Match(TokenType::SEMICOLON); // Optional semicolon

        return std::make_unique<SelectStatement>(table_name_str, select_list, std::move(where_filter), std::move(group_by_list), has_count_star, std::move(aggregations), std::move(having_filter), std::move(subquery), table_alias, std::vector<CommonTableExpression>{}, std::move(window_functions));
    }

} // namespace Database