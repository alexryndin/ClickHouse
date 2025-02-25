#pragma once

#include <Parsers/IParserBase.h>
#include <Parsers/ExpressionElementParsers.h>

namespace DB
{

/** Query like this:
  * ALTER TABLE [db.]name [ON CLUSTER cluster]
  *     [ADD COLUMN [IF NOT EXISTS] col_name type [AFTER col_after],]
  *     [DROP COLUMN [IF EXISTS] col_to_drop, ...]
  *     [CLEAR COLUMN [IF EXISTS] col_to_clear [IN PARTITION partition],]
  *     [MODIFY COLUMN [IF EXISTS] col_to_modify type, ...]
  *     [MODIFY PRIMARY KEY (a, b, c...)]
  *     [COMMENT COLUMN [IF EXISTS] col_name string]
  *     [DROP|DETACH|ATTACH PARTITION|PART partition, ...]
  *     [FETCH PARTITION partition FROM ...]
  *     [FREEZE [PARTITION] [WITH NAME name]]
  *     [DELETE WHERE ...]
  *     [UPDATE col_name = expr, ... WHERE ...]
  * ALTER LIVE VIEW [db.name]
  *     [REFRESH]
  */

class ParserAlterQuery : public IParserBase
{
protected:
    const char * getName() const { return "ALTER query"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected);
};


class ParserAlterCommandList : public IParserBase
{
protected:
    const char * getName() const { return "a list of ALTER commands"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected);

public:
    bool is_live_view;

    ParserAlterCommandList(bool is_live_view_ = false) : is_live_view(is_live_view_) {}
};


class ParserAlterCommand : public IParserBase
{
protected:
    const char * getName() const { return "ALTER command"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected);

public:
    bool is_live_view;

    ParserAlterCommand(bool is_live_view_ = false) : is_live_view(is_live_view_) {}
};


/// Part of the UPDATE command of the form: col_name = expr
class ParserAssignment : public IParserBase
{
protected:
    const char * getName() const { return "column assignment"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected);
};

}
