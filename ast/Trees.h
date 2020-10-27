#ifndef SORBET_TREES_H
#define SORBET_TREES_H

#include "common/common.h"
#include "core/Context.h"
#include "core/LocalVariable.h"
#include "core/SymbolRef.h"
#include "core/Types.h"
#include <memory>
#include <vector>

//
// This file defines the IR that most of the middle phases of Sorbet operate on
// and manipulate. It aims to be a middle ground between the parser output
// (very verbose and fine grained) and the CFG data structure (very easy to
// typecheck but very hard to do ad-hoc transformations on).
//
// This IR is best learned by example. Try using the `--print` option to sorbet
// on a handful of test/testdata files. Since there are multiple phases that
// return this IR, there are multiple valid print options which will show you
// an ast::Expression.
//
// Another good way to discover things is to grep for the class name in the
// various *-raw.exp snapshot tests to fine a test file that uses that node.
// Keep in mind that this IR is meant to be somewhat coarse grained, so one
// node type can likely have been created from multiple Ruby constructs.
//

namespace sorbet::ast {

enum class Tag {
    EmptyTree = 1,
    Send,
    ClassDef,
    MethodDef,
    If,
    While,
    Break,
    Retry,
    Next,
    Return,
    RescueCase,
    Rescue,
    Local,
    UnresolvedIdent,
    RestArg,
    KeywordArg,
    OptionalArg,
    BlockArg,
    ShadowArg,
    Assign,
    Cast,
    Hash,
    Array,
    Literal,
    UnresolvedConstantLit,
    ConstantLit,
    ZSuperArgs,
    Block,
    InsSeq,
};

// A mapping from tree type to its corresponding tag.
template <typename T> struct TreeToTag;

// A mapping from tag value to the tree it represents.
template <Tag T> struct TagToTree;

class Expression;

class TreePtr {
public:
    // We store tagged pointers as 64-bit values.
    using tagged_storage = u8;

private:
    static constexpr tagged_storage TAG_MASK = 0xffff000000000007;

    static constexpr tagged_storage PTR_MASK = ~TAG_MASK;

    tagged_storage ptr;

    template <typename E, typename... Args> friend TreePtr make_tree(Args &&...);

    static tagged_storage tagPtr(Tag tag, void *expr) {
        auto val = static_cast<tagged_storage>(tag);
        if (val >= 8) {
            // Store the tag in the upper 16 bits of the pointer, as it won't fit in the lower three bits.
            val <<= 48;
        }

        auto maskedPtr = reinterpret_cast<tagged_storage>(expr) & PTR_MASK;

        return maskedPtr | val;
    }

    explicit TreePtr(Tag tag, void *expr) : ptr(tagPtr(tag, expr)) {}

    static void deleteTagged(Tag tag, void *ptr) noexcept;

    // A version of release that doesn't mask the tag bits
    tagged_storage releaseTagged() noexcept {
        auto saved = ptr;
        ptr = 0;
        return saved;
    }

    // A version of reset that expects the tagged bits to be set.
    void resetTagged(tagged_storage expr) noexcept {
        Tag tagVal;
        void *saved = nullptr;

        if (ptr != 0) {
            tagVal = tag();
            saved = get();
        }

        ptr = expr;

        if (saved != nullptr) {
            deleteTagged(tagVal, saved);
        }
    }

public:
    constexpr TreePtr() noexcept : ptr(0) {}

    TreePtr(std::nullptr_t) noexcept : TreePtr() {}

    // Construction from a tagged pointer. This is needed for:
    // * ResolveConstantsWalk::isFullyResolved
    explicit TreePtr(tagged_storage ptr) : ptr(ptr) {}

    ~TreePtr() {
        if (ptr != 0) {
            deleteTagged(tag(), get());
        }
    }

    TreePtr(const TreePtr &) = delete;
    TreePtr &operator=(const TreePtr &) = delete;

    TreePtr(TreePtr &&other) noexcept {
        ptr = other.releaseTagged();
    }

    TreePtr &operator=(TreePtr &&other) noexcept {
        if (*this == other) {
            return *this;
        }

        resetTagged(other.releaseTagged());
        return *this;
    }

    void *release() noexcept {
        auto *saved = get();
        ptr = 0;
        return saved;
    }

    void reset() noexcept {
        resetTagged(0);
    }

    void reset(std::nullptr_t) noexcept {
        resetTagged(0);
    }

    template <typename T> void reset(T *expr = nullptr) noexcept {
        resetTagged(tagPtr(TreeToTag<T>::value, expr));
    }

    Tag tag() const noexcept {
        ENFORCE(ptr != 0);

        auto value = reinterpret_cast<tagged_storage>(ptr) & TAG_MASK;
        if (value <= 7) {
            return static_cast<Tag>(value);
        } else {
            return static_cast<Tag>(value >> 48);
        }
    }

    Expression *get() const noexcept {
        auto val = ptr & PTR_MASK;

        if constexpr (sizeof(void *) == 4) {
            return reinterpret_cast<Expression *>(val);
        } else {
            // sign extension for the upper 16 bits
            return reinterpret_cast<Expression *>((val << 16) >> 16);
        }
    }

    // Fetch the tagged pointer. This is needed for:
    // * ResolveConstantsWalk::isFullyResolved
    tagged_storage getTagged() const noexcept {
        return ptr;
    }

    Expression *operator->() const noexcept {
        return get();
    }

    Expression *operator*() const noexcept {
        return get();
    }

    explicit operator bool() const noexcept {
        return get() != nullptr;
    }

    bool operator==(const TreePtr &other) const noexcept {
        return get() == other.get();
    }

    bool operator!=(const TreePtr &other) const noexcept {
        return get() != other.get();
    }

    TreePtr deepCopy() const;
};

template <class E, typename... Args> TreePtr make_tree(Args &&... args) {
    return TreePtr(TreeToTag<E>::value, new E(std::forward<Args>(args)...));
}

class Expression {
public:
    Expression(core::LocOffsets loc);
    virtual ~Expression() = default;
    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const = 0;
    std::string toString(const core::GlobalState &gs) const {
        return toStringWithTabs(gs);
    }
    virtual std::string nodeName() = 0;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0) = 0;
    virtual void _sanityCheck() = 0;
    const core::LocOffsets loc;

    bool isSelfReference() const;
};
CheckSize(Expression, 16, 8);

struct ParsedFile {
    TreePtr tree;
    core::FileRef file;
};

/**
 * Stores a vector of `ParsedFile`s. May be empty if pass was canceled or encountered an error.
 * TODO: Modify to store reason if we ever have multiple reasons for a pass to stop. Currently, it's only empty if the
 * pass is canceled in LSP mode.
 */
class ParsedFilesOrCancelled final {
private:
    std::optional<std::vector<ParsedFile>> trees;

public:
    ParsedFilesOrCancelled();
    ParsedFilesOrCancelled(std::vector<ParsedFile> &&trees);

    bool hasResult() const;
    std::vector<ParsedFile> &result();
};

template <class To> bool isa_tree(const TreePtr &what) {
    return what != nullptr && what.tag() == TreeToTag<To>::value;
}

bool isa_reference(const TreePtr &what);

bool isa_declaration(const TreePtr &what);

template <class To> To *cast_tree(TreePtr &what) {
    if (isa_tree<To>(what)) {
        return reinterpret_cast<To *>(what.get());
    } else {
        return nullptr;
    }
}

template <class To> const To *cast_tree(const TreePtr &what) {
    if (isa_tree<To>(what)) {
        return reinterpret_cast<To *>(what.get());
    } else {
        return nullptr;
    }
}

template <class To> To &cast_tree_nonnull(TreePtr &what) {
    ENFORCE(isa_tree<To>(what), "cast_tree_nonnull failed!");
    return *reinterpret_cast<To *>(what.get());
}

template <class To> const To &cast_tree_nonnull(const TreePtr &what) {
    ENFORCE(isa_tree<To>(what), "cast_tree_nonnull failed!");
    return *reinterpret_cast<To *>(what.get());
}

class Reference : public Expression {
public:
    Reference(core::LocOffsets loc);
};
CheckSize(Reference, 16, 8);

class Declaration : public Expression {
public:
    core::Loc declLoc;
    core::SymbolRef symbol;

    Declaration(core::LocOffsets loc, core::Loc declLoc, core::SymbolRef symbol);
};
CheckSize(Declaration, 32, 8);

#define TREE(name)                                                                  \
    class name;                                                                     \
    template <> struct TreeToTag<name> { static constexpr Tag value = Tag::name; }; \
    template <> struct TagToTree<Tag::name> { using value = name; };                \
    class __attribute__((aligned(8))) name final

TREE(ClassDef) : public Declaration {
public:
    enum class Kind : u1 {
        Module,
        Class,
    };
    Kind kind;
    static constexpr int EXPECTED_RHS_COUNT = 4;
    using RHS_store = InlinedVector<TreePtr, EXPECTED_RHS_COUNT>;

    RHS_store rhs;
    TreePtr name;
    // For unresolved names. Once they are typeAlias to Symbols they go into the Symbol

    static constexpr int EXPECTED_ANCESTORS_COUNT = 2;
    using ANCESTORS_store = InlinedVector<TreePtr, EXPECTED_ANCESTORS_COUNT>;

    ANCESTORS_store ancestors;
    ANCESTORS_store singletonAncestors;

    ClassDef(core::LocOffsets loc, core::Loc declLoc, core::SymbolRef symbol, TreePtr name, ANCESTORS_store ancestors,
             RHS_store rhs, ClassDef::Kind kind);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(ClassDef, 136, 8);

TREE(MethodDef) : public Declaration {
public:
    TreePtr rhs;

    using ARGS_store = InlinedVector<TreePtr, core::SymbolRef::EXPECTED_METHOD_ARGS_COUNT>;
    ARGS_store args;

    core::NameRef name;

    struct Flags {
        bool isSelfMethod : 1;
        bool isRewriterSynthesized : 1;

        // In C++20 we can replace this with bit field initialzers
        Flags() : isSelfMethod(false), isRewriterSynthesized(false) {}
    };
    CheckSize(Flags, 1, 1);

    Flags flags;

    MethodDef(core::LocOffsets loc, core::Loc declLoc, core::SymbolRef symbol, core::NameRef name, ARGS_store args,
              TreePtr rhs, Flags flags);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(MethodDef, 72, 8);

TREE(If) : public Expression {
public:
    TreePtr cond;
    TreePtr thenp;
    TreePtr elsep;

    If(core::LocOffsets loc, TreePtr cond, TreePtr thenp, TreePtr elsep);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(If, 40, 8);

TREE(While) : public Expression {
public:
    TreePtr cond;
    TreePtr body;

    While(core::LocOffsets loc, TreePtr cond, TreePtr body);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(While, 32, 8);

TREE(Break) : public Expression {
public:
    TreePtr expr;

    Break(core::LocOffsets loc, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Break, 24, 8);

TREE(Retry) : public Expression {
public:
    Retry(core::LocOffsets loc);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Retry, 16, 8);

TREE(Next) : public Expression {
public:
    TreePtr expr;

    Next(core::LocOffsets loc, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Next, 24, 8);

TREE(Return) : public Expression {
public:
    TreePtr expr;

    Return(core::LocOffsets loc, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Return, 24, 8);

TREE(RescueCase) : public Expression {
public:
    static constexpr int EXPECTED_EXCEPTION_COUNT = 2;
    using EXCEPTION_store = InlinedVector<TreePtr, EXPECTED_EXCEPTION_COUNT>;

    EXCEPTION_store exceptions;

    // If present, var is always an UnresolvedIdent[kind=Local] up until the
    // namer, at which point it is a Local.
    TreePtr var;
    TreePtr body;

    RescueCase(core::LocOffsets loc, EXCEPTION_store exceptions, TreePtr var, TreePtr body);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(RescueCase, 56, 8);

TREE(Rescue) : public Expression {
public:
    static constexpr int EXPECTED_RESCUE_CASE_COUNT = 2;
    using RESCUE_CASE_store = InlinedVector<TreePtr, EXPECTED_RESCUE_CASE_COUNT>;

    TreePtr body;
    RESCUE_CASE_store rescueCases;
    TreePtr else_;
    TreePtr ensure;

    Rescue(core::LocOffsets loc, TreePtr body, RESCUE_CASE_store rescueCases, TreePtr else_, TreePtr ensure);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Rescue, 64, 8);

TREE(Local) : public Reference {
public:
    core::LocalVariable localVariable;

    Local(core::LocOffsets loc, core::LocalVariable localVariable1);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Local, 24, 8);

TREE(UnresolvedIdent) : public Reference {
public:
    enum class Kind : u1 {
        Local,
        Instance,
        Class,
        Global,
    };
    core::NameRef name;
    Kind kind;

    UnresolvedIdent(core::LocOffsets loc, Kind kind, core::NameRef name);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(UnresolvedIdent, 24, 8);

TREE(RestArg) : public Reference {
public:
    TreePtr expr;

    RestArg(core::LocOffsets loc, TreePtr arg);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(RestArg, 24, 8);

TREE(KeywordArg) : public Reference {
public:
    TreePtr expr;

    KeywordArg(core::LocOffsets loc, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(KeywordArg, 24, 8);

TREE(OptionalArg) : public Reference {
public:
    TreePtr expr;
    TreePtr default_;

    OptionalArg(core::LocOffsets loc, TreePtr expr, TreePtr default_);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(OptionalArg, 32, 8);

TREE(BlockArg) : public Reference {
public:
    TreePtr expr;

    BlockArg(core::LocOffsets loc, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(BlockArg, 24, 8);

TREE(ShadowArg) : public Reference {
public:
    TreePtr expr;

    ShadowArg(core::LocOffsets loc, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(ShadowArg, 24, 8);

TREE(Assign) : public Expression {
public:
    TreePtr lhs;
    TreePtr rhs;

    Assign(core::LocOffsets loc, TreePtr lhs, TreePtr rhs);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Assign, 32, 8);

TREE(Send) : public Expression {
public:
    core::NameRef fun;

    struct Flags {
        bool isPrivateOk : 1;
        bool isRewriterSynthesized : 1;

        // In C++20 we can replace this with bit field initialzers
        Flags() : isPrivateOk(false), isRewriterSynthesized(false) {}
    };
    CheckSize(Flags, 1, 1);

    Flags flags;

    u2 numPosArgs;

    TreePtr recv;

    static constexpr int EXPECTED_ARGS_COUNT = 2;
    using ARGS_store = InlinedVector<TreePtr, EXPECTED_ARGS_COUNT>;
    ARGS_store args;

    TreePtr block; // null if no block passed

    Send(core::LocOffsets loc, TreePtr recv, core::NameRef fun, u2 numPosArgs, ARGS_store args, TreePtr block = nullptr,
         Flags flags = {});

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

    std::pair<int, int> kwArgsRange() const {
        auto res = std::make_pair<int, int>(numPosArgs, args.size());
        if (hasKwSplat()) {
            res.second = res.second - 1;
        }
        return res;
    }

    // True when there are keyword args, but false when there is just a keyword splat.
    bool hasKwArgs() const {
        return ((args.size() - numPosArgs) & ~0x1);
    }

    bool hasKwSplat() const {
        return (args.size() - numPosArgs) & 0x1;
    }

private:
    virtual void _sanityCheck();
};
CheckSize(Send, 64, 8);

TREE(Cast) : public Expression {
public:
    // The name of the cast operator.
    core::NameRef cast;

    core::TypePtr type;
    TreePtr arg;

    Cast(core::LocOffsets loc, core::TypePtr ty, TreePtr arg, core::NameRef cast);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Cast, 48, 8);

TREE(Hash) : public Expression {
public:
    static constexpr int EXPECTED_ENTRY_COUNT = 2;
    using ENTRY_store = InlinedVector<TreePtr, EXPECTED_ENTRY_COUNT>;

    ENTRY_store keys;
    ENTRY_store values;

    Hash(core::LocOffsets loc, ENTRY_store keys, ENTRY_store values);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Hash, 64, 8);

TREE(Array) : public Expression {
public:
    static constexpr int EXPECTED_ENTRY_COUNT = 4;
    using ENTRY_store = InlinedVector<TreePtr, EXPECTED_ENTRY_COUNT>;

    ENTRY_store elems;

    Array(core::LocOffsets loc, ENTRY_store elems);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(Array, 56, 8);

TREE(Literal) : public Expression {
public:
    core::TypePtr value;

    Literal(core::LocOffsets loc, const core::TypePtr &value);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();
    bool isString(const core::GlobalState &gs) const;
    bool isSymbol(const core::GlobalState &gs) const;
    bool isNil(const core::GlobalState &gs) const;
    core::NameRef asString(const core::GlobalState &gs) const;
    core::NameRef asSymbol(const core::GlobalState &gs) const;
    bool isTrue(const core::GlobalState &gs) const;
    bool isFalse(const core::GlobalState &gs) const;

private:
    virtual void _sanityCheck();
};
CheckSize(Literal, 32, 8);

TREE(UnresolvedConstantLit) : public Expression {
public:
    core::NameRef cnst;
    TreePtr scope;

    UnresolvedConstantLit(core::LocOffsets loc, TreePtr scope, core::NameRef cnst);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(UnresolvedConstantLit, 32, 8);

TREE(ConstantLit) : public Expression {
public:
    core::SymbolRef symbol; // If this is a normal constant. This symbol may be already dealiased.
    // For constants that failed resolution, symbol will be set to StubModule and resolutionScopes
    // will be set to whatever nesting scope we estimate the constant could have been defined in.
    using ResolutionScopes = InlinedVector<core::SymbolRef, 1>;
    ResolutionScopes resolutionScopes;
    TreePtr original;

    ConstantLit(core::LocOffsets loc, core::SymbolRef symbol, TreePtr original);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();
    std::optional<std::pair<core::SymbolRef, std::vector<core::NameRef>>> fullUnresolvedPath(
        const core::GlobalState &gs) const;

private:
    virtual void _sanityCheck();
};
CheckSize(ConstantLit, 56, 8);

TREE(ZSuperArgs) : public Expression {
public:
    // null if no block passed
    ZSuperArgs(core::LocOffsets loc);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(ZSuperArgs, 16, 8);

TREE(Block) : public Expression {
public:
    MethodDef::ARGS_store args;
    TreePtr body;

    Block(core::LocOffsets loc, MethodDef::ARGS_store args, TreePtr body);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();
    virtual void _sanityCheck();
};
CheckSize(Block, 48, 8);

TREE(InsSeq) : public Expression {
public:
    static constexpr int EXPECTED_STATS_COUNT = 4;
    using STATS_store = InlinedVector<TreePtr, EXPECTED_STATS_COUNT>;
    // Statements
    STATS_store stats;

    // The distinguished final expression (determines return value)
    TreePtr expr;

    InsSeq(core::LocOffsets locOffsets, STATS_store stats, TreePtr expr);

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(InsSeq, 64, 8);

TREE(EmptyTree) : public Expression {
public:
    EmptyTree();

    TreePtr deepCopy() const;

    virtual std::string toStringWithTabs(const core::GlobalState &gs, int tabs = 0) const;
    virtual std::string showRaw(const core::GlobalState &gs, int tabs = 0);
    virtual std::string nodeName();

private:
    virtual void _sanityCheck();
};
CheckSize(EmptyTree, 16, 8);

// This specialization of make_tree exists to ensure that we only ever create one empty tree.
template <> TreePtr make_tree<EmptyTree>();

/** https://git.corp.stripe.com/gist/nelhage/51564501674174da24822e60ad770f64
 *
 *  [] - prototype only
 *
 *                 / Control Flow <- while, if, for, break, next, retry, return, rescue, case
 * Pre-CFG-Node <-
 *                 \ Instruction <- assign, send, [new], ident, named_arg, hash, array, literals(symbols, ints, floats,
 * strings, constants, nil), constants(resolver will desugar it into literals), array_splat(*), hash_splat(**), self,
 * insseq, Block)
 *
 *                  \ Definition  <-  class(name, parent, mixins, body)
 *                                    module
 *                                    def
 *                                    defself
 *                                    const_assign
 *
 *
 *
 * know id for: top, bottom, kernel?, basicobject, class, module [postponed], unit, Hash, Array, String, Symbol, float,
 * int, numeric, double, unknown
 *
 *
 *
 * Desugar string concatenation into series of .to_s calls and string concatenations
 */

} // namespace sorbet::ast

#endif // SORBET_TREES_H
