#include "common/Timer.h"
#include "core/Names.h"
#include "core/core.h"
#include "core/errors/resolver.h"
#include "resolver/resolver.h"

#include "absl/algorithm/container.h"

#include <map>
#include <vector>

using namespace std;

namespace sorbet::resolver {

namespace {
core::SymbolRef dealiasAt(const core::GlobalState &gs, core::SymbolRef tparam, core::SymbolRef klass,
                          const vector<vector<pair<core::SymbolRef, core::SymbolRef>>> &typeAliases) {
    ENFORCE(tparam.data(gs)->isTypeMember());
    if (tparam.data(gs)->owner == klass) {
        return tparam;
    } else {
        core::SymbolRef cursor;
        if (tparam.data(gs)->owner.data(gs)->derivesFrom(gs, klass)) {
            cursor = tparam.data(gs)->owner;
        } else if (klass.data(gs)->derivesFrom(gs, tparam.data(gs)->owner)) {
            cursor = klass;
        }
        while (true) {
            if (!cursor.exists()) {
                return cursor;
            }
            for (auto aliasPair : typeAliases[cursor.classOrModuleIndex()]) {
                if (aliasPair.first == tparam) {
                    return dealiasAt(gs, aliasPair.second, klass, typeAliases);
                }
            }
            cursor = cursor.data(gs)->superClass();
        }
    }
}

bool resolveTypeMember(core::GlobalState &gs, core::SymbolRef parent, core::SymbolRef parentTypeMember,
                       core::SymbolRef sym, vector<vector<pair<core::SymbolRef, core::SymbolRef>>> &typeAliases) {
    core::NameRef name = parentTypeMember.data(gs)->name;
    core::SymbolRef my = sym.data(gs)->findMember(gs, name);
    if (!my.exists()) {
        auto code =
            parent == core::Symbols::Enumerable() || parent.data(gs)->derivesFrom(gs, core::Symbols::Enumerable())
                ? core::errors::Resolver::EnumerableParentTypeNotDeclared
                : core::errors::Resolver::ParentTypeNotDeclared;

        if (auto e = gs.beginError(sym.data(gs)->loc(), code)) {
            e.setHeader("Type `{}` declared by parent `{}` must be re-declared in `{}`", name.show(gs),
                        parent.data(gs)->show(gs), sym.data(gs)->show(gs));
            e.addErrorLine(parentTypeMember.data(gs)->loc(), "`{}` declared in parent here", name.show(gs));
        }
        my = gs.enterTypeMember(sym.data(gs)->loc(), sym, name, core::Variance::Invariant);
        my.data(gs)->setFixed();
        auto untyped = core::Types::untyped(gs, sym);
        my.data(gs)->resultType = core::make_type<core::LambdaParam>(my, untyped, untyped);
        return false;
    }
    const auto &data = my.data(gs);
    if (!data->isTypeMember() && !data->isTypeArgument()) {
        if (auto e = gs.beginError(data->loc(), core::errors::Resolver::NotATypeVariable)) {
            e.setHeader("Type variable `{}` needs to be declared as `= type_member(SOMETHING)`", name.show(gs));
        }
        auto synthesizedName = gs.freshNameUnique(core::UniqueNameKind::TypeVarName, name, 1);
        my = gs.enterTypeMember(sym.data(gs)->loc(), sym, synthesizedName, core::Variance::Invariant);
        my.data(gs)->setFixed();
        auto untyped = core::Types::untyped(gs, sym);
        my.data(gs)->resultType = core::make_type<core::LambdaParam>(my, untyped, untyped);
        return false;
    }
    auto myVariance = data->variance();
    auto parentVariance = parentTypeMember.data(gs)->variance();
    if (!sym.data(gs)->derivesFrom(gs, core::Symbols::Class()) && myVariance != parentVariance &&
        myVariance != core::Variance::Invariant) {
        if (auto e = gs.beginError(data->loc(), core::errors::Resolver::ParentVarianceMismatch)) {
            e.setHeader("Type variance mismatch with parent `{}`", parent.data(gs)->show(gs));
        }
        return false;
    }
    typeAliases[sym.classOrModuleIndex()].emplace_back(parentTypeMember, my);
    return true;
} // namespace

void resolveTypeMembers(core::GlobalState &gs, core::SymbolRef sym,
                        vector<vector<pair<core::SymbolRef, core::SymbolRef>>> &typeAliases, vector<bool> &resolved) {
    ENFORCE(sym.data(gs)->isClassOrModule());
    if (resolved[sym.classOrModuleIndex()]) {
        return;
    }
    resolved[sym.classOrModuleIndex()] = true;

    if (sym.data(gs)->superClass().exists()) {
        auto parent = sym.data(gs)->superClass();
        resolveTypeMembers(gs, parent, typeAliases, resolved);

        auto tps = parent.data(gs)->typeMembers();
        bool foundAll = true;
        for (core::SymbolRef tp : tps) {
            bool foundThis = resolveTypeMember(gs, parent, tp, sym, typeAliases);
            foundAll = foundAll && foundThis;
        }
        if (foundAll) {
            int i = 0;
            // check that type params are in the same order.
            for (core::SymbolRef tp : tps) {
                core::SymbolRef my = dealiasAt(gs, tp, sym, typeAliases);
                ENFORCE(my.exists(), "resolver failed to register type member aliases");
                if (sym.data(gs)->typeMembers()[i] != my) {
                    if (auto e = gs.beginError(my.data(gs)->loc(), core::errors::Resolver::TypeMembersInWrongOrder)) {
                        e.setHeader("Type members in wrong order");
                    }
                    int foundIdx = 0;
                    while (foundIdx < sym.data(gs)->typeMembers().size() &&
                           sym.data(gs)->typeMembers()[foundIdx] != my) {
                        foundIdx++;
                    }
                    ENFORCE(foundIdx < sym.data(gs)->typeMembers().size());
                    // quadratic
                    swap(sym.data(gs)->typeMembers()[foundIdx], sym.data(gs)->typeMembers()[i]);
                }
                i++;
            }
        }
    }
    auto mixins = sym.data(gs)->mixins();
    for (core::SymbolRef mixin : mixins) {
        resolveTypeMembers(gs, mixin, typeAliases, resolved);
        auto typeMembers = mixin.data(gs)->typeMembers();
        for (core::SymbolRef tp : typeMembers) {
            resolveTypeMember(gs, mixin, tp, sym, typeAliases);
        }
    }

    if (sym.data(gs)->isClassOrModuleClass()) {
        for (core::SymbolRef tp : sym.data(gs)->typeMembers()) {
            // AttachedClass is covariant, but not controlled by the user.
            if (tp.data(gs)->name == core::Names::Constants::AttachedClass()) {
                continue;
            }

            auto myVariance = tp.data(gs)->variance();
            if (myVariance != core::Variance::Invariant) {
                auto loc = tp.data(gs)->loc();
                if (!loc.file().data(gs).isPayload()) {
                    if (auto e = gs.beginError(loc, core::errors::Resolver::VariantTypeMemberInClass)) {
                        e.setHeader("Classes can only have invariant type members");
                    }
                    return;
                }
            }
        }
    }

    // If this class has no type members, fix attached class early.
    if (sym.data(gs)->typeMembers().empty()) {
        auto singleton = sym.data(gs)->lookupSingletonClass(gs);
        if (singleton.exists()) {
            // AttachedClass doesn't exist on `T.untyped`, which is a problem
            // with RuntimeProfiled.
            auto attachedClass = singleton.data(gs)->findMember(gs, core::Names::Constants::AttachedClass());
            if (attachedClass.exists()) {
                auto *lambdaParam = core::cast_type<core::LambdaParam>(attachedClass.data(gs)->resultType.get());
                ENFORCE(lambdaParam != nullptr);

                lambdaParam->lowerBound = core::Types::bottom();
                lambdaParam->upperBound = sym.data(gs)->externalType(gs);
            }
        }
    }
}

}; // namespace

void Resolver::finalizeAncestors(core::GlobalState &gs) {
    Timer timer(gs.tracer(), "resolver.finalize_ancestors");
    int methodCount = 0;
    int classCount = 0;
    int moduleCount = 0;
    for (int i = 0; i < gs.methodsUsed(); ++i) {
        auto ref = core::SymbolRef(&gs, core::SymbolRef::Kind::Method, i);
        ENFORCE(ref.data(gs)->isMethod());
        auto loc = ref.data(gs)->loc();
        if (loc.file().exists() && loc.file().data(gs).sourceType == core::File::Type::Normal) {
            methodCount++;
        }
    }
    for (int i = 1; i < gs.classAndModulesUsed(); ++i) {
        auto ref = core::SymbolRef(&gs, core::SymbolRef::Kind::ClassOrModule, i);
        ENFORCE(ref.data(gs)->isClassOrModule());
        if (!ref.data(gs)->isClassModuleSet()) {
            // we did not see a declaration for this type not did we see it used. Default to module.
            ref.data(gs)->setIsModule(true);
        }
        auto loc = ref.data(gs)->loc();
        if (loc.file().exists() && loc.file().data(gs).sourceType == core::File::Type::Normal) {
            if (ref.data(gs)->isClassOrModuleClass()) {
                classCount++;
            } else {
                moduleCount++;
            }
        }
        if (ref.data(gs)->superClass().exists() && ref.data(gs)->superClass() != core::Symbols::todo()) {
            continue;
        }
        if (ref == core::Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass()) {
            // only happens if we run without stdlib
            ENFORCE(!core::Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass().data(gs)->loc().exists());
            ref.data(gs)->setSuperClass(core::Symbols::BasicObject());
            continue;
        }

        auto attached = ref.data(gs)->attachedClass(gs);
        bool isSingleton = attached.exists() && attached != core::Symbols::untyped();
        if (isSingleton) {
            if (attached == core::Symbols::BasicObject()) {
                ref.data(gs)->setSuperClass(core::Symbols::Class());
            } else if (attached.data(gs)->superClass() ==
                       core::Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass()) {
                // Note: this depends on attached classes having lower indexes in name table than their singletons
                ref.data(gs)->setSuperClass(core::Symbols::Module());
            } else {
                ENFORCE(attached.data(gs)->superClass() != core::Symbols::todo());
                auto singleton = attached.data(gs)->superClass().data(gs)->singletonClass(gs);
                ref.data(gs)->setSuperClass(singleton);
            }
        } else {
            if (ref.data(gs)->isClassOrModuleClass()) {
                if (!core::Symbols::Object().data(gs)->derivesFrom(gs, ref) && core::Symbols::Object() != ref) {
                    ref.data(gs)->setSuperClass(core::Symbols::Object());
                }
            } else {
                if (!core::Symbols::BasicObject().data(gs)->derivesFrom(gs, ref) &&
                    core::Symbols::BasicObject() != ref) {
                    ref.data(gs)->setSuperClass(core::Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass());
                }
            }
        }
    }

    prodCounterAdd("types.input.modules.total", moduleCount);
    prodCounterAdd("types.input.classes.total", classCount);
    prodCounterAdd("types.input.methods.total", methodCount);
}

struct ParentLinearizationInformation {
    const InlinedVector<core::SymbolRef, 4> &mixins;
    core::SymbolRef superClass;
    core::SymbolRef klass;
    InlinedVector<core::SymbolRef, 4> fullLinearizationSlow(core::GlobalState &gs);
};

int maybeAddMixin(core::GlobalState &gs, core::SymbolRef forSym, InlinedVector<core::SymbolRef, 4> &mixinList,
                  core::SymbolRef mixin, core::SymbolRef parent, int pos) {
    if (forSym == mixin) {
        Exception::raise("Loop in mixins");
    }
    if (parent.data(gs)->derivesFrom(gs, mixin)) {
        return pos;
    }
    auto fnd = find(mixinList.begin(), mixinList.end(), mixin);
    if (fnd != mixinList.end()) {
        auto newPos = fnd - mixinList.begin();
        if (newPos >= pos) {
            return newPos + 1;
        }
        return pos;
    } else {
        mixinList.insert(mixinList.begin() + pos, mixin);
        return pos + 1;
    }
}

// ** This implements Dmitry's understanding of Ruby linerarization with an optimization that common
// tails of class linearization aren't copied around.
// In order to obtain Ruby-side ancestors, one would need to walk superclass chain and concatenate `mixins`.
// The algorithm is harder to explain than to code, so just follow code & tests if `testdata/resolver/linearization`
ParentLinearizationInformation computeClassLinearization(core::GlobalState &gs, core::SymbolRef ofClass) {
    ENFORCE_NO_TIMER(ofClass.exists());
    auto data = ofClass.data(gs);
    ENFORCE_NO_TIMER(data->isClassOrModule());
    if (!data->isClassOrModuleLinearizationComputed()) {
        if (data->superClass().exists()) {
            computeClassLinearization(gs, data->superClass());
        }
        InlinedVector<core::SymbolRef, 4> currentMixins = data->mixins();
        InlinedVector<core::SymbolRef, 4> newMixins;
        for (auto mixin : currentMixins) {
            if (mixin == data->superClass()) {
                continue;
            }
            if (mixin.data(gs)->superClass() == core::Symbols::StubSuperClass() ||
                mixin.data(gs)->superClass() == core::Symbols::StubModule()) {
                newMixins.emplace_back(mixin);
                continue;
            }
            ENFORCE_NO_TIMER(mixin.data(gs)->isClassOrModule());
            ParentLinearizationInformation mixinLinearization = computeClassLinearization(gs, mixin);

            if (!mixin.data(gs)->isClassOrModuleModule()) {
                if (mixin != core::Symbols::BasicObject()) {
                    if (auto e = gs.beginError(data->loc(), core::errors::Resolver::IncludesNonModule)) {
                        e.setHeader("Only modules can be `{}`d. This module or class includes `{}`", "include",
                                    mixin.data(gs)->show(gs));
                    }
                }
                // insert all transitive parents of class to bring methods back.
                auto allMixins = mixinLinearization.fullLinearizationSlow(gs);
                newMixins.insert(newMixins.begin(), allMixins.begin(), allMixins.end());
            } else {
                int pos = 0;
                pos = maybeAddMixin(gs, ofClass, newMixins, mixin, data->superClass(), pos);
                for (auto &mixinLinearizationComponent : mixinLinearization.mixins) {
                    pos = maybeAddMixin(gs, ofClass, newMixins, mixinLinearizationComponent, data->superClass(), pos);
                }
            }
        }
        data->mixins() = std::move(newMixins);
        data->setClassOrModuleLinearizationComputed();
        if (debug_mode) {
            for (auto oldMixin : currentMixins) {
                ENFORCE(ofClass.data(gs)->derivesFrom(gs, oldMixin), "{} no longer derives from {}",
                        ofClass.data(gs)->showFullName(gs), oldMixin.data(gs)->showFullName(gs));
            }
        }
    }
    ENFORCE_NO_TIMER(data->isClassOrModuleLinearizationComputed());
    return ParentLinearizationInformation{data->mixins(), data->superClass(), ofClass};
}

void fullLinearizationSlowImpl(core::GlobalState &gs, const ParentLinearizationInformation &info,
                               InlinedVector<core::SymbolRef, 4> &acc) {
    ENFORCE(!absl::c_linear_search(acc, info.klass));
    acc.emplace_back(info.klass);

    for (auto m : info.mixins) {
        if (!absl::c_linear_search(acc, m)) {
            if (m.data(gs)->isClassOrModuleModule()) {
                acc.emplace_back(m);
            } else {
                fullLinearizationSlowImpl(gs, computeClassLinearization(gs, m), acc);
            }
        }
    }
    if (info.superClass.exists()) {
        if (!absl::c_linear_search(acc, info.superClass)) {
            fullLinearizationSlowImpl(gs, computeClassLinearization(gs, info.superClass), acc);
        }
    }
};
InlinedVector<core::SymbolRef, 4> ParentLinearizationInformation::fullLinearizationSlow(core::GlobalState &gs) {
    InlinedVector<core::SymbolRef, 4> res;
    fullLinearizationSlowImpl(gs, *this, res);
    return res;
}

void Resolver::computeLinearization(core::GlobalState &gs) {
    Timer timer(gs.tracer(), "resolver.compute_linearization");

    // TODO: this does not support `prepend`
    for (int i = 1; i < gs.classAndModulesUsed(); ++i) {
        const auto &ref = core::SymbolRef(&gs, core::SymbolRef::Kind::ClassOrModule, i);
        ENFORCE(ref.data(gs)->isClassOrModule());
        computeClassLinearization(gs, ref);
    }
}

void Resolver::finalizeSymbols(core::GlobalState &gs) {
    Timer timer(gs.tracer(), "resolver.finalize_resolution");
    // TODO(nelhage): Properly this first loop should go in finalizeAncestors,
    // but we currently compute mixes_in_class_methods during the same AST walk
    // that resolves types and we don't want to introduce additional passes if
    // we don't have to. It would be a tractable refactor to merge it
    // `ResolveConstantsWalk` if it becomes necessary to process earlier.
    for (int i = 1; i < gs.classAndModulesUsed(); ++i) {
        auto sym = core::SymbolRef(&gs, core::SymbolRef::Kind::ClassOrModule, i);
        ENFORCE(sym.data(gs)->isClassOrModule());

        core::SymbolRef singleton;
        for (auto ancst : sym.data(gs)->mixins()) {
            auto classMethods = ancst.data(gs)->findMember(gs, core::Names::classMethods());
            if (!classMethods.exists()) {
                continue;
            }
            if (!singleton.exists()) {
                singleton = sym.data(gs)->singletonClass(gs);
            }
            singleton.data(gs)->addMixin(classMethods);
        }
    }

    computeLinearization(gs);

    vector<vector<pair<core::SymbolRef, core::SymbolRef>>> typeAliases;
    typeAliases.resize(gs.classAndModulesUsed());
    vector<bool> resolved;
    resolved.resize(gs.classAndModulesUsed());
    for (int i = 1; i < gs.classAndModulesUsed(); ++i) {
        auto sym = core::SymbolRef(&gs, core::SymbolRef::Kind::ClassOrModule, i);
        ENFORCE(sym.data(gs)->isClassOrModule());
        resolveTypeMembers(gs, sym, typeAliases, resolved);
    }
}

} // namespace sorbet::resolver
