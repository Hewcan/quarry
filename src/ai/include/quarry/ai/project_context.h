#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>
#include <vector>

// Application-code hints for the AI (Laravel first): which model owns a table and which PHP enum
// backs a status-like column, so a request such as "orders that are pending" can be answered
// without the user naming the column or its raw value. Pure text in, text out: this module never
// reads the project folder; the app hands it the source of each file. All strings are untrusted.
namespace quarry::ai {

struct EnumCast
{
    QString column;
    QString enumName; // short class name, as written in the model
};

struct LaravelModel
{
    QString className;
    QString table; // declared `$table`, or the Eloquent default when tableInferred
    bool tableInferred = false;
    std::vector<EnumCast> enumCasts; // every `'col' => Some::class` cast; filtered on render
};

struct PhpEnumCase
{
    QString name;
    QString value; // empty for a pure (non-backed) enum
};

struct PhpEnum
{
    QString name;
    QString backingType; // "string", "int" or empty
    std::vector<PhpEnumCase> cases;
};

struct ProjectContext
{
    std::vector<LaravelModel> models;
    std::vector<PhpEnum> enums;
};

// nullopt when the file declares no class. Comments are ignored.
std::optional<LaravelModel> parseLaravelModel(QStringView php);
// nullopt when the file declares no enum.
std::optional<PhpEnum> parsePhpEnum(QStringView php);
// Eloquent's default table: `OrderItem` -> `order_items`.
QString defaultTableName(QStringView className);

// One line per table in `tables` (qualified names are matched by their last part) that has a
// model, plus one line per enum those models cast to. Enum cases (their values come from code,
// like ENUM values from the schema) only go out with includeEnumCases. Empty when nothing
// matches; capped at maxChars.
QString renderProjectContext(const ProjectContext &project, const QStringList &tables,
                             bool includeEnumCases, qsizetype maxChars = 8000);

} // namespace quarry::ai
