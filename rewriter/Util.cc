#include "rewriter/Util.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/core.h"
#include "rewriter/rewriter.h"

using namespace std;

namespace sorbet::rewriter {

ast::TreePtr ASTUtil::dupType(const ast::TreePtr &orig) {
    auto send = ast::cast_tree<ast::Send>(orig);
    if (send) {
        ast::Send::ARGS_store args;
        auto dupRecv = dupType(send->recv);
        if (!dupRecv) {
            return nullptr;
        }
        if (send->fun == core::Names::enum_()) {
            // T.enum() is weird, and accepts values instead of types. Just copy
            // it blindly through.
            return send->deepCopy();
        }

        if (send->fun == core::Names::params() && send->numPosArgs == 0 && send->args.size() % 2 == 0) {
            // T.proc.params takes inlined keyword argument pairs, and can't handle kwsplat
            ast::Send::ARGS_store args;

            for (auto i = 0; i < send->args.size(); i += 2) {
                ENFORCE(ast::isa_tree<ast::Literal>(send->args[i]));
                args.emplace_back(send->args[i].deepCopy());

                auto dupedValue = ASTUtil::dupType(send->args[i + 1]);
                if (dupedValue == nullptr) {
                    return nullptr;
                }

                args.emplace_back(std::move(dupedValue));
            }

            return ast::MK::Send(send->loc, std::move(dupRecv), send->fun, 0, std::move(args));
        }

        for (auto &arg : send->args) {
            auto dupArg = dupType(arg);
            if (!dupArg) {
                // This isn't a Type signature, bail out
                return nullptr;
            }
            args.emplace_back(std::move(dupArg));
        }
        return ast::MK::Send(send->loc, std::move(dupRecv), send->fun, send->numPosArgs, std::move(args));
    }

    auto *ident = ast::cast_tree<ast::ConstantLit>(orig);
    if (ident) {
        auto orig = dupType(ident->original);
        if (ident->original && !orig) {
            return nullptr;
        }
        return ast::make_tree<ast::ConstantLit>(ident->loc, ident->symbol, std::move(orig));
    }

    auto *cons = ast::cast_tree<ast::UnresolvedConstantLit>(orig);
    if (!cons) {
        return nullptr;
    }

    auto *scopeCnst = ast::cast_tree<ast::UnresolvedConstantLit>(cons->scope);
    if (!scopeCnst) {
        if (ast::isa_tree<ast::EmptyTree>(cons->scope)) {
            return ast::MK::UnresolvedConstant(cons->loc, ast::MK::EmptyTree(), cons->cnst);
        }
        auto *id = ast::cast_tree<ast::ConstantLit>(cons->scope);
        if (id == nullptr) {
            return nullptr;
        }
        ENFORCE(id->symbol == core::Symbols::root());
        return ast::MK::UnresolvedConstant(cons->loc, dupType(cons->scope), cons->cnst);
    }
    auto scope = dupType(cons->scope);
    if (scope == nullptr) {
        return nullptr;
    }
    return ast::MK::UnresolvedConstant(cons->loc, std::move(scope), cons->cnst);
}

bool ASTUtil::hasHashValue(core::MutableContext ctx, const ast::Hash &hash, core::NameRef name) {
    for (const auto &keyExpr : hash.keys) {
        auto *key = ast::cast_tree<ast::Literal>(keyExpr);
        if (key && key->isSymbol(ctx) && key->asSymbol(ctx) == name) {
            return true;
        }
    }
    return false;
}

bool ASTUtil::hasTruthyHashValue(core::MutableContext ctx, const ast::Hash &hash, core::NameRef name) {
    int i = -1;
    for (const auto &keyExpr : hash.keys) {
        i++;
        auto *key = ast::cast_tree<ast::Literal>(keyExpr);
        if (key && key->isSymbol(ctx) && key->asSymbol(ctx) == name) {
            auto *val = ast::cast_tree<ast::Literal>(hash.values[i]);
            if (!val) {
                // All non-literals are truthy
                return true;
            }
            if (val->isNil(ctx) || val->isFalse(ctx)) {
                return false;
            }
            return true;
        }
    }
    return false;
}

pair<ast::TreePtr, ast::TreePtr> ASTUtil::extractHashValue(core::MutableContext ctx, ast::Hash &hash,
                                                           core::NameRef name) {
    int i = -1;
    for (auto &keyExpr : hash.keys) {
        i++;
        auto *key = ast::cast_tree<ast::Literal>(keyExpr);
        if (key && key->isSymbol(ctx) && key->asSymbol(ctx) == name) {
            auto key = std::move(keyExpr);
            auto value = std::move(hash.values[i]);
            hash.keys.erase(hash.keys.begin() + i);
            hash.values.erase(hash.values.begin() + i);
            return make_pair(move(key), move(value));
        }
    }
    return make_pair(nullptr, nullptr);
}

ast::Send *ASTUtil::castSig(ast::TreePtr &expr) {
    auto *send = ast::cast_tree<ast::Send>(expr);
    if (send == nullptr) {
        return nullptr;
    }

    return ASTUtil::castSig(send);
}

// This will return nullptr if the argument is not the right shape as a sig (i.e. a send to a method called `sig` with 0
// or 1 arguments, that in turn contains a block that contains a send) and it also checks the final method of the send
// against the provided `returns` (so that some uses can specifically look for `void` sigs while others can specifically
// look for non-void sigs).
ast::Send *ASTUtil::castSig(ast::Send *send) {
    if (send->fun != core::Names::sig()) {
        return nullptr;
    }
    if (send->block.get() == nullptr) {
        return nullptr;
    }
    // 0 args is common case
    // 1 arg  is `sig(:final)`
    // 2 args is `Sorbet::Private::Static.sig(self, :final)`
    if (send->args.size() > 2) {
        return nullptr;
    }
    auto *block = ast::cast_tree<ast::Block>(send->block);
    ENFORCE(block);
    auto *body = ast::cast_tree<ast::Send>(block->body);
    while (body != nullptr && (body->fun == core::Names::checked() || body->fun == core::Names::onFailure())) {
        body = ast::cast_tree<ast::Send>(body->recv);
    }
    if (body != nullptr && (body->fun == core::Names::void_() || body->fun == core::Names::returns())) {
        return send;
    } else {
        return nullptr;
    }
}

ast::TreePtr ASTUtil::mkKwArgsHash(const ast::Send *send) {
    if (send->args.empty()) {
        return nullptr;
    }

    ast::Hash::ENTRY_store keys;
    ast::Hash::ENTRY_store values;

    auto [kwStart, kwEnd] = send->kwArgsRange();
    for (auto i = kwStart; i < kwEnd; i += 2) {
        keys.emplace_back(send->args[i].deepCopy());
        values.emplace_back(send->args[i + 1].deepCopy());
    }

    // handle a double-splat or a hash literal as the last argument
    bool explicitEmptyHash = false;
    if (send->hasKwSplat() || !send->hasKwArgs()) {
        if (auto *hash = ast::cast_tree<ast::Hash>(send->args.back())) {
            explicitEmptyHash = hash->keys.empty();
            for (auto i = 0; i < hash->keys.size(); ++i) {
                keys.emplace_back(hash->keys[i].deepCopy());
                values.emplace_back(hash->values[i].deepCopy());
            }
        }
    }

    if (!keys.empty() || explicitEmptyHash) {
        return ast::MK::Hash(send->loc, std::move(keys), std::move(values));
    } else {
        return nullptr;
    }
}

ast::TreePtr ASTUtil::mkGet(core::Context ctx, core::LocOffsets loc, core::NameRef name, ast::TreePtr rhs) {
    return ast::MK::SyntheticMethod0(loc, core::Loc(ctx.file, loc), name, move(rhs));
}

ast::TreePtr ASTUtil::mkSet(core::Context ctx, core::LocOffsets loc, core::NameRef name, core::LocOffsets argLoc,
                            ast::TreePtr rhs) {
    return ast::MK::SyntheticMethod1(loc, core::Loc(ctx.file, loc), name, ast::MK::Local(argLoc, core::Names::arg0()),
                                     move(rhs));
}

ast::TreePtr ASTUtil::mkNilable(core::LocOffsets loc, ast::TreePtr type) {
    return ast::MK::Send1(loc, ast::MK::T(loc), core::Names::nilable(), move(type));
}

namespace {

// Returns `true` when the expression passed is an UnresolvedConstantLit with the name `Kernel` and no additional scope.
bool isKernel(const ast::TreePtr &expr) {
    if (auto *constRecv = ast::cast_tree<ast::UnresolvedConstantLit>(expr)) {
        return ast::isa_tree<ast::EmptyTree>(constRecv->scope) && constRecv->cnst == core::Names::Constants::Kernel();
    }
    return false;
}

} // namespace

ast::TreePtr ASTUtil::thunkBody(core::MutableContext ctx, ast::TreePtr &node) {
    auto *send = ast::cast_tree<ast::Send>(node);
    if (send == nullptr) {
        return nullptr;
    }
    if (send->fun != core::Names::lambda() && send->fun != core::Names::proc()) {
        return nullptr;
    }
    // Valid receivers for lambda/proc are either a self reference or `Kernel`
    if (!send->recv->isSelfReference() && !isKernel(send->recv)) {
        return nullptr;
    }
    if (send->block == nullptr) {
        return nullptr;
    }
    auto *block = ast::cast_tree<ast::Block>(send->block);
    if (!block->args.empty()) {
        return nullptr;
    }
    return std::move(block->body);
}

} // namespace sorbet::rewriter
