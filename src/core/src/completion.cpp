#include "quarry/completion.h"

#include <QSet>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>

namespace quarry {
namespace {

struct Candidate
{
    int group = 0; // 0: starts with the prefix, 1: contains it
    int rank = 0; // position of the kind in the scope's order
    QString key; // lowercased name
    QString name;
    SuggestionKind kind = SuggestionKind::Keyword;
    QString detail;
    const CachedObject *relation = nullptr; // Table/View: the detail comes from its path
};

SuggestionKind kindOf(ObjectKind kind)
{
    return kind == ObjectKind::Table ? SuggestionKind::Table : SuggestionKind::View;
}

// Kinds offered in a scope, in display order.
std::vector<SuggestionKind> kindOrder(CompletionScope scope)
{
    using K = SuggestionKind;
    switch (scope) {
    case CompletionScope::Keyword:
        return { K::Keyword, K::Function, K::Column, K::Table, K::View };
    case CompletionScope::Table:
        return { K::Table, K::View, K::Schema };
    case CompletionScope::Column:
        return { K::Column, K::Function, K::Keyword };
    case CompletionScope::Member:
        return { K::Column, K::Table, K::View };
    case CompletionScope::None:
        break;
    }
    return {};
}

bool isPlainIdentifier(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (qsizetype i = 0; i < name.size(); ++i) {
        const char16_t c = name[i].unicode();
        const bool letter = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_';
        const bool digit = c >= u'0' && c <= u'9';
        if (!(letter || (digit && i > 0))) {
            return false;
        }
    }
    return true;
}

QString pathKey(const QStringList &parts)
{
    return parts.join(QChar(0x1f)).toLower();
}

// Tables of the statement that the cache knows, without repeats.
std::vector<ObjectPath> resolvedTables(const SqlContext &ctx, const MetadataCache &cache)
{
    std::vector<ObjectPath> out;
    QSet<QString> seen;
    for (const TableReference &ref : ctx.tables) {
        if (const std::optional<ObjectPath> path = cache.resolve(ref.parts)) {
            if (!seen.contains(pathKey(path->parts))) {
                seen.insert(pathKey(path->parts));
                out.push_back(*path);
            }
        }
    }
    return out;
}

// The table a Member qualifier names: an alias of the statement, else a table of the cache.
std::optional<ObjectPath> qualifierTable(const SqlContext &ctx, const MetadataCache &cache)
{
    if (ctx.qualifier.size() == 1) {
        for (const TableReference &ref : ctx.tables) {
            if (!ref.alias.isEmpty()
                && ref.alias.compare(ctx.qualifier.first(), Qt::CaseInsensitive) == 0) {
                return cache.resolve(ref.parts);
            }
        }
    }
    return cache.resolve(ctx.qualifier);
}

// Whether `path` without its last `drop` parts ends with `parts`, ignoring case.
bool endsWithParts(const QStringList &path, const QStringList &parts, qsizetype drop = 0)
{
    if (path.size() - drop < parts.size()) {
        return false;
    }
    const qsizetype offset = path.size() - drop - parts.size();
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (path.at(offset + i).compare(parts.at(i), Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    return true;
}

class Collector
{
public:
    Collector(const SqlContext &ctx, int limit)
        : m_limit(limit)
        , m_prefix(ctx.prefix.toLower())
        , m_order(kindOrder(ctx.scope))
    { }

    // -1: no match; 0: prefix; 1: substring.
    int groupOf(const QString &key) const
    {
        if (key.startsWith(m_prefix)) {
            return 0;
        }
        return !m_prefix.isEmpty() && key.contains(m_prefix) ? 1 : -1;
    }

    int rankOf(SuggestionKind kind) const
    {
        const auto it = std::find(m_order.begin(), m_order.end(), kind);
        return it == m_order.end() ? -1 : static_cast<int>(it - m_order.begin());
    }

    void add(Candidate c) { m_candidates.push_back(std::move(c)); }

    void addNamed(SuggestionKind kind, const QString &name, const QString &detail)
    {
        const int rank = rankOf(kind);
        if (rank < 0) {
            return;
        }
        QString key = name.toLower();
        const int group = groupOf(key);
        if (group >= 0) {
            add({ group, rank, std::move(key), name, kind, detail, nullptr });
        }
    }

    // Walks the sorted index: the prefix range by binary search, then (only if it does not fill
    // the limit) the substring matches. Per (group, kind) at most `limit` candidates are kept.
    template<typename Filter> void addRelations(const MetadataCache &cache, Filter &&accept)
    {
        const auto &relations = cache.relations();
        const auto &keys = cache.relationKeys();
        const auto &order = cache.relationOrder();
        std::array<std::array<int, 6>, 2> counts {};
        auto consider = [&](qsizetype index, int group) {
            const CachedObject &object = relations[static_cast<size_t>(index)];
            if (!accept(object)) {
                return false;
            }
            const SuggestionKind kind = kindOf(object.path.kind);
            const int rank = rankOf(kind);
            if (rank < 0
                || counts[static_cast<size_t>(group)][static_cast<size_t>(rank)] >= m_limit) {
                return true;
            }
            ++counts[static_cast<size_t>(group)][static_cast<size_t>(rank)];
            add({ group, rank, keys[static_cast<size_t>(index)], object.name, kind, QString(),
                  &object });
            return true;
        };

        auto it = std::partition_point(order.begin(), order.end(), [&](qsizetype i) {
            return keys[static_cast<size_t>(i)] < m_prefix;
        });
        qsizetype matched = 0;
        const auto prefixBegin = it;
        for (; it != order.end() && keys[static_cast<size_t>(*it)].startsWith(m_prefix); ++it) {
            matched += consider(*it, 0) ? 1 : 0;
        }
        if (m_prefix.isEmpty() || matched >= m_limit) {
            return;
        }
        auto scanSubstrings = [&](auto first, auto last) {
            for (; first != last; ++first) {
                if (keys[static_cast<size_t>(*first)].contains(m_prefix)) {
                    consider(*first, 1);
                }
            }
        };
        scanSubstrings(order.begin(), prefixBegin);
        scanSubstrings(it, order.end());
    }

    std::vector<Suggestion> finish(const ISqlDialect &dialect)
    {
        auto less = [](const Candidate &a, const Candidate &b) {
            if (a.group != b.group) {
                return a.group < b.group;
            }
            if (a.rank != b.rank) {
                return a.rank < b.rank;
            }
            if (a.key != b.key) {
                return a.key < b.key;
            }
            if (a.name != b.name) {
                return a.name < b.name;
            }
            if (a.detail != b.detail) {
                return a.detail < b.detail;
            }
            return std::less<const CachedObject *>()(a.relation, b.relation); // load order
        };
        const auto take = static_cast<size_t>(
            std::min<qsizetype>(static_cast<qsizetype>(m_candidates.size()), m_limit));
        std::partial_sort(m_candidates.begin(), m_candidates.begin() + static_cast<qsizetype>(take),
                          m_candidates.end(), less);
        std::vector<Suggestion> out;
        out.reserve(take);
        std::optional<QSet<QString>> keywords; // lowercased, built only if a name needs checking
        for (size_t i = 0; i < take; ++i) {
            const Candidate &c = m_candidates[i];
            Suggestion s;
            s.kind = c.kind;
            s.label = c.kind == SuggestionKind::Keyword ? c.name.toUpper() : c.name;
            s.detail = c.detail;
            if (c.relation != nullptr) {
                const QStringList &parts = c.relation->path.parts;
                s.detail = parts.size() >= 2 ? parts.at(parts.size() - 2) : QString();
            }
            if (c.kind == SuggestionKind::Keyword || c.kind == SuggestionKind::Function) {
                s.text = s.label;
            } else {
                bool quote = !isPlainIdentifier(c.name);
                if (!quote) {
                    if (!keywords) {
                        keywords.emplace();
                        for (const QString &k : dialect.keywords()) {
                            keywords->insert(k.toLower());
                        }
                    }
                    quote = keywords->contains(c.key);
                }
                s.text = quote ? dialect.quoteIdentifier(c.name) : c.name;
            }
            out.push_back(std::move(s));
        }
        return out;
    }

private:
    qsizetype m_limit;
    QString m_prefix;
    std::vector<SuggestionKind> m_order;
    std::vector<Candidate> m_candidates;
};

} // namespace

std::vector<Suggestion> CompletionEngine::complete(const SqlContext &ctx,
                                                   const MetadataCache &cache,
                                                   const ISqlDialect &dialect, int limit)
{
    if (ctx.scope == CompletionScope::None || limit <= 0) {
        return {};
    }
    Collector collector(ctx, limit);
    const CompletionScope scope = ctx.scope;

    auto addColumns = [&](const ObjectPath &table, bool withTable) {
        if (const std::vector<CachedObject> *columns = cache.columns(table)) {
            const QString tableName = table.parts.isEmpty() ? QString() : table.parts.last();
            for (const CachedObject &column : *columns) {
                QString detail = column.detail;
                if (withTable) {
                    detail
                        = detail.isEmpty() ? tableName : tableName + QStringLiteral(": ") + detail;
                }
                collector.addNamed(SuggestionKind::Column, column.name, detail);
            }
        }
    };

    if (scope == CompletionScope::Member) {
        if (const std::optional<ObjectPath> table = qualifierTable(ctx, cache)) {
            addColumns(*table, false);
        } else {
            collector.addRelations(cache, [&](const CachedObject &object) {
                return endsWithParts(object.path.parts, ctx.qualifier, 1);
            });
        }
    } else {
        if (scope == CompletionScope::Keyword || scope == CompletionScope::Column) {
            for (const ObjectPath &table : resolvedTables(ctx, cache)) {
                addColumns(table, true);
            }
            for (const QString &name : dialect.builtinFunctions()) {
                collector.addNamed(SuggestionKind::Function, name, QString());
            }
            for (const QString &name : dialect.keywords()) {
                collector.addNamed(SuggestionKind::Keyword, name, QString());
            }
        }
        if (scope == CompletionScope::Keyword || scope == CompletionScope::Table) {
            collector.addRelations(cache, [](const CachedObject &) { return true; });
        }
        if (scope == CompletionScope::Table) {
            for (const CachedObject &schema : cache.schemas()) {
                collector.addNamed(SuggestionKind::Schema, schema.name, QString());
            }
        }
    }
    return collector.finish(dialect);
}

std::vector<ObjectPath> CompletionEngine::missingColumns(const SqlContext &ctx,
                                                         const MetadataCache &cache)
{
    std::vector<ObjectPath> out;
    auto consider = [&](const ObjectPath &path) {
        const bool known = std::any_of(out.begin(), out.end(),
                                       [&](const ObjectPath &p) { return p.parts == path.parts; });
        if (!known && cache.columns(path) == nullptr) {
            out.push_back(path);
        }
    };
    for (const TableReference &ref : ctx.tables) {
        if (const std::optional<ObjectPath> path = cache.resolve(ref.parts)) {
            consider(*path);
        }
    }
    if (ctx.scope == CompletionScope::Member && !ctx.qualifier.isEmpty()) {
        if (const std::optional<ObjectPath> path = qualifierTable(ctx, cache)) {
            consider(*path);
        }
    }
    return out;
}

} // namespace quarry
