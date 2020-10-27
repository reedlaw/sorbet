#include "GlobalState.h"

#include "common/Timer.h"
#include "common/sort.h"
#include "core/Error.h"
#include "core/Hashing.h"
#include "core/NameHash.h"
#include "core/Names.h"
#include "core/Names_gen.h"
#include "core/Types.h"
#include "core/Unfreeze.h"
#include "core/errors/errors.h"
#include "core/lsp/Task.h"
#include "core/lsp/TypecheckEpochManager.h"
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "core/ErrorQueue.h"
#include "core/errors/infer.h"
#include "main/pipeline/semantic_extension/SemanticExtension.h"

template class std::vector<std::pair<unsigned int, unsigned int>>;
template class std::shared_ptr<sorbet::core::GlobalState>;
template class std::unique_ptr<sorbet::core::GlobalState>;

using namespace std;

namespace sorbet::core {

SymbolRef GlobalState::synthesizeClass(NameRef nameId, u4 superclass, bool isModule) {
    // This can't use enterClass since there is a chicken and egg problem.
    // These will be added to Symbols::root().members later.
    SymbolRef symRef = SymbolRef(this, SymbolRef::Kind::ClassOrModule, classAndModules.size());
    classAndModules.emplace_back();
    SymbolData data = symRef.dataAllowingNone(*this); // allowing noSymbol is needed because this enters noSymbol.
    data->name = nameId;
    data->owner = Symbols::root();
    data->flags = 0;
    data->setClassOrModule();
    data->setIsModule(isModule);
    data->setSuperClass(SymbolRef(this, SymbolRef::Kind::ClassOrModule, superclass));

    if (symRef.classOrModuleIndex() > Symbols::root().classOrModuleIndex()) {
        Symbols::root().dataAllowingNone(*this)->members()[nameId] = symRef;
    }
    return symRef;
}

atomic<int> globalStateIdCounter(1);
const int Symbols::MAX_PROC_ARITY;

GlobalState::GlobalState(shared_ptr<ErrorQueue> errorQueue)
    : GlobalState(move(errorQueue), make_shared<lsp::TypecheckEpochManager>()) {}

GlobalState::GlobalState(shared_ptr<ErrorQueue> errorQueue, shared_ptr<lsp::TypecheckEpochManager> epochManager)
    : globalStateId(globalStateIdCounter.fetch_add(1)), errorQueue(std::move(errorQueue)),
      lspQuery(lsp::Query::noQuery()), epochManager(move(epochManager)) {
    // Reserve memory in internal vectors for the contents of payload.
    names.reserve(PAYLOAD_MAX_NAME_COUNT);
    classAndModules.reserve(PAYLOAD_MAX_CLASS_AND_MODULE_COUNT);
    methods.reserve(PAYLOAD_MAX_METHOD_COUNT);
    fields.reserve(PAYLOAD_MAX_FIELD_COUNT);
    typeArguments.reserve(PAYLOAD_MAX_TYPE_ARGUMENT_COUNT);
    typeMembers.reserve(PAYLOAD_MAX_TYPE_MEMBER_COUNT);

    int namesByHashSize = 2 * PAYLOAD_MAX_NAME_COUNT;
    namesByHash.resize(namesByHashSize);
    ENFORCE((namesByHashSize & (namesByHashSize - 1)) == 0, "namesByHashSize is not a power of 2");
}

void GlobalState::initEmpty() {
    UnfreezeFileTable fileTableAccess(*this);
    UnfreezeNameTable nameTableAccess(*this);
    UnfreezeSymbolTable symTableAccess(*this);
    names.emplace_back(); // first name is used in hashes to indicate empty cell
    names[0].kind = NameKind::UTF8;
    names[0].raw.utf8 = string_view();
    Names::registerNames(*this);

    SymbolRef id;
    id = synthesizeClass(core::Names::Constants::NoSymbol(), 0);
    ENFORCE(id == Symbols::noSymbol());
    id = synthesizeClass(core::Names::Constants::Top(), 0);
    ENFORCE(id == Symbols::top());
    id = synthesizeClass(core::Names::Constants::Bottom(), 0);
    ENFORCE(id == Symbols::bottom());
    id = synthesizeClass(core::Names::Constants::Root(), 0);
    ENFORCE(id == Symbols::root());
    id = core::Symbols::root().data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::rootSingleton());
    id = synthesizeClass(core::Names::Constants::Todo(), 0);
    ENFORCE(id == Symbols::todo());
    id = synthesizeClass(core::Names::Constants::Object(), Symbols::BasicObject().classOrModuleIndex());
    ENFORCE(id == Symbols::Object());
    id = synthesizeClass(core::Names::Constants::Integer());
    ENFORCE(id == Symbols::Integer());
    id = synthesizeClass(core::Names::Constants::Float());
    ENFORCE(id == Symbols::Float());
    id = synthesizeClass(core::Names::Constants::String());
    ENFORCE(id == Symbols::String());
    id = synthesizeClass(core::Names::Constants::Symbol());
    ENFORCE(id == Symbols::Symbol());
    id = synthesizeClass(core::Names::Constants::Array());
    ENFORCE(id == Symbols::Array());
    id = synthesizeClass(core::Names::Constants::Hash());
    ENFORCE(id == Symbols::Hash());
    id = synthesizeClass(core::Names::Constants::TrueClass());
    ENFORCE(id == Symbols::TrueClass());
    id = synthesizeClass(core::Names::Constants::FalseClass());
    ENFORCE(id == Symbols::FalseClass());
    id = synthesizeClass(core::Names::Constants::NilClass());
    ENFORCE(id == Symbols::NilClass());
    id = synthesizeClass(core::Names::Constants::Untyped(), 0);
    ENFORCE(id == Symbols::untyped());
    id = synthesizeClass(core::Names::Constants::Opus(), 0, true);
    ENFORCE(id == Symbols::Opus());
    id = synthesizeClass(core::Names::Constants::T(), Symbols::todo().classOrModuleIndex(), true);
    ENFORCE(id == Symbols::T());
    id = synthesizeClass(core::Names::Constants::Class(), 0);
    ENFORCE(id == Symbols::Class());
    id = synthesizeClass(core::Names::Constants::BasicObject(), 0);
    ENFORCE(id == Symbols::BasicObject());
    id = synthesizeClass(core::Names::Constants::Kernel(), 0, true);
    ENFORCE(id == Symbols::Kernel());
    id = synthesizeClass(core::Names::Constants::Range());
    ENFORCE(id == Symbols::Range());
    id = synthesizeClass(core::Names::Constants::Regexp());
    ENFORCE(id == Symbols::Regexp());
    id = synthesizeClass(core::Names::Constants::Magic());
    ENFORCE(id == Symbols::Magic());
    id = Symbols::Magic().data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::MagicSingleton());
    id = synthesizeClass(core::Names::Constants::Module());
    ENFORCE(id == Symbols::Module());
    id = synthesizeClass(core::Names::Constants::StandardError());
    ENFORCE(id == Symbols::StandardError());
    id = synthesizeClass(core::Names::Constants::Complex());
    ENFORCE(id == Symbols::Complex());
    id = synthesizeClass(core::Names::Constants::Rational());
    ENFORCE(id == Symbols::Rational());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Array());
    ENFORCE(id == Symbols::T_Array());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Hash());
    ENFORCE(id == Symbols::T_Hash());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Proc());
    ENFORCE(id == Symbols::T_Proc());
    id = synthesizeClass(core::Names::Constants::Proc());
    ENFORCE(id == Symbols::Proc());
    id = synthesizeClass(core::Names::Constants::Enumerable(), 0, true);
    ENFORCE(id == Symbols::Enumerable());
    id = synthesizeClass(core::Names::Constants::Set());
    ENFORCE(id == Symbols::Set());
    id = synthesizeClass(core::Names::Constants::Struct());
    ENFORCE(id == Symbols::Struct());
    id = synthesizeClass(core::Names::Constants::File());
    ENFORCE(id == Symbols::File());
    id = synthesizeClass(core::Names::Constants::Sorbet());
    ENFORCE(id == Symbols::Sorbet());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet(), core::Names::Constants::Private());
    ENFORCE(id == Symbols::Sorbet_Private());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private(), core::Names::Constants::Static());
    ENFORCE(id == Symbols::Sorbet_Private_Static());
    id = Symbols::Sorbet_Private_Static().data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::Sorbet_Private_StaticSingleton());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::StubModule());
    ENFORCE(id == Symbols::StubModule());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::StubMixin());
    ENFORCE(id == Symbols::StubMixin());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::StubSuperClass());
    ENFORCE(id == Symbols::StubSuperClass());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Enumerable());
    ENFORCE(id == Symbols::T_Enumerable());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Range());
    ENFORCE(id == Symbols::T_Range());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Set());
    ENFORCE(id == Symbols::T_Set());
    id = synthesizeClass(core::Names::Constants::Configatron());
    ENFORCE(id == Symbols::Configatron());
    id = enterClassSymbol(Loc::none(), Symbols::Configatron(), core::Names::Constants::Store());
    ENFORCE(id == Symbols::Configatron_Store());
    id = enterClassSymbol(Loc::none(), Symbols::Configatron(), core::Names::Constants::RootStore());
    ENFORCE(id == Symbols::Configatron_RootStore());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::Void());
    id.data(*this)->setIsModule(false);
    ENFORCE(id == Symbols::void_());
    id = synthesizeClass(core::Names::Constants::TypeAlias(), 0);
    ENFORCE(id == Symbols::typeAliasTemp());
    id = synthesizeClass(core::Names::Constants::Chalk(), 0, true);
    ENFORCE(id == Symbols::Chalk());
    id = enterClassSymbol(Loc::none(), Symbols::Chalk(), core::Names::Constants::Tools());
    ENFORCE(id == Symbols::Chalk_Tools());
    id = enterClassSymbol(Loc::none(), Symbols::Chalk_Tools(), core::Names::Constants::Accessible());
    ENFORCE(id == Symbols::Chalk_Tools_Accessible());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Generic());
    ENFORCE(id == Symbols::T_Generic());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::Tuple());
    ENFORCE(id == Symbols::Tuple());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::Shape());
    ENFORCE(id == Symbols::Shape());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::Subclasses());
    ENFORCE(id == Symbols::Subclasses());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(),
                          core::Names::Constants::ImplicitModuleSuperclass());
    ENFORCE(id == Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass());
    id = enterClassSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::Constants::ReturnTypeInference());
    ENFORCE(id == Symbols::Sorbet_Private_Static_ReturnTypeInference());
    id =
        enterMethodSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::guessedTypeTypeParameterHolder());
    ENFORCE(id == Symbols::Sorbet_Private_Static_ReturnTypeInference_guessed_type_type_parameter_holder());
    {
        auto &arg = enterMethodArgumentSymbol(
            Loc::none(), Symbols::Sorbet_Private_Static_ReturnTypeInference_guessed_type_type_parameter_holder(),
            Names::blkArg());
        arg.flags.isBlock = true;
    }
    id = enterTypeArgument(
        Loc::none(), Symbols::Sorbet_Private_Static_ReturnTypeInference_guessed_type_type_parameter_holder(),
        freshNameUnique(core::UniqueNameKind::TypeVarName, core::Names::Constants::InferredReturnType(), 1),
        core::Variance::ContraVariant);
    id.data(*this)->resultType = make_type<core::TypeVar>(id);
    ENFORCE(
        id ==
        Symbols::Sorbet_Private_Static_ReturnTypeInference_guessed_type_type_parameter_holder_tparam_contravariant());
    id = enterTypeArgument(
        Loc::none(), Symbols::Sorbet_Private_Static_ReturnTypeInference_guessed_type_type_parameter_holder(),
        freshNameUnique(core::UniqueNameKind::TypeVarName, core::Names::Constants::InferredArgumentType(), 1),
        core::Variance::CoVariant);
    id.data(*this)->resultType = make_type<core::TypeVar>(id);
    ENFORCE(id ==
            Symbols::Sorbet_Private_Static_ReturnTypeInference_guessed_type_type_parameter_holder_tparam_covariant());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Sig());
    ENFORCE(id == Symbols::T_Sig());

    // A magic non user-creatable class with methods to keep state between passes
    id = enterFieldSymbol(Loc::none(), Symbols::Magic(), core::Names::Constants::UndeclaredFieldStub());
    ENFORCE(id == Symbols::Magic_undeclaredFieldStub());

    // Sorbet::Private::Static#badAliasMethodStub(*arg0 : T.untyped) => T.untyped
    id = enterMethodSymbol(Loc::none(), Symbols::Sorbet_Private_Static(), core::Names::badAliasMethodStub());
    ENFORCE(id == Symbols::Sorbet_Private_Static_badAliasMethodStub());
    id.data(*this)->resultType = Types::untyped(*this, id);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), Symbols::Sorbet_Private_Static_badAliasMethodStub(),
                                              core::Names::arg0());
        arg.flags.isRepeated = true;
        arg.type = Types::untyped(*this, id);
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), Symbols::Sorbet_Private_Static_badAliasMethodStub(),
                                              core::Names::blkArg());
        arg.flags.isBlock = true;
        arg.type = Types::untyped(*this, id);
    }

    // T::Helpers
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Helpers());
    ENFORCE(id == Symbols::T_Helpers());

    // SigBuilder magic class
    id = synthesizeClass(core::Names::Constants::DeclBuilderForProcs());
    ENFORCE(id == Symbols::DeclBuilderForProcs());
    id = Symbols::DeclBuilderForProcs().data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::DeclBuilderForProcsSingleton());

    // Ruby 2.5 Hack
    id = synthesizeClass(core::Names::Constants::Net(), 0, true);
    ENFORCE(id == Symbols::Net());
    id = enterClassSymbol(Loc::none(), Symbols::Net(), core::Names::Constants::IMAP());
    Symbols::Net_IMAP().data(*this)->setIsModule(false);
    ENFORCE(id == Symbols::Net_IMAP());
    id = enterClassSymbol(Loc::none(), Symbols::Net(), core::Names::Constants::Protocol());
    ENFORCE(id == Symbols::Net_Protocol());
    Symbols::Net_Protocol().data(*this)->setIsModule(false);

    id = enterClassSymbol(Loc::none(), Symbols::T_Sig(), core::Names::Constants::WithoutRuntime());
    ENFORCE(id == Symbols::T_Sig_WithoutRuntime());

    id = synthesizeClass(core::Names::Constants::Enumerator());
    ENFORCE(id == Symbols::Enumerator());
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Enumerator());
    ENFORCE(id == Symbols::T_Enumerator());

    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Struct());
    ENFORCE(id == Symbols::T_Struct());

    id = synthesizeClass(core::Names::Constants::Singleton(), 0, true);
    ENFORCE(id == Symbols::Singleton());

    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Enum());
    id.data(*this)->setIsModule(false);
    ENFORCE(id == Symbols::T_Enum());

    // T::Sig#sig
    id = enterMethodSymbol(Loc::none(), Symbols::T_Sig(), Names::sig());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::arg0());
        arg.flags.isDefault = true;
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::blkArg());
        arg.flags.isBlock = true;
    }
    ENFORCE(id == Symbols::sig());

    // Enumerable::Lazy
    id = enterClassSymbol(Loc::none(), Symbols::Enumerator(), core::Names::Constants::Lazy());
    ENFORCE(id == Symbols::Enumerator_Lazy());

    id = enterClassSymbol(Loc::none(), Symbols::T(), Names::Constants::Private());
    ENFORCE(id == Symbols::T_Private());
    id = enterClassSymbol(Loc::none(), Symbols::T_Private(), Names::Constants::Types());
    ENFORCE(id == Symbols::T_Private_Types());
    id = enterClassSymbol(Loc::none(), Symbols::T_Private_Types(), Names::Constants::Void());
    id.data(*this)->setIsModule(false);
    ENFORCE(id == Symbols::T_Private_Types_Void());
    id = enterClassSymbol(Loc::none(), Symbols::T_Private_Types_Void(), Names::Constants::VOID());
    ENFORCE(id == Symbols::T_Private_Types_Void_VOID());
    id = id.data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::T_Private_Types_Void_VOIDSingleton());

    // T.class_of(T::Sig::WithoutRuntime)
    id = Symbols::T_Sig_WithoutRuntime().data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::T_Sig_WithoutRuntimeSingleton());

    // T::Sig::WithoutRuntime.sig
    id = enterMethodSymbol(Loc::none(), Symbols::T_Sig_WithoutRuntimeSingleton(), Names::sig());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::arg0());
        arg.flags.isDefault = true;
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::blkArg());
        arg.flags.isBlock = true;
    }
    ENFORCE(id == Symbols::sigWithoutRuntime());

    id = enterClassSymbol(Loc::none(), Symbols::T(), Names::Constants::NonForcingConstants());
    ENFORCE(id == Symbols::T_NonForcingConstants());

    id = enterClassSymbol(Loc::none(), Symbols::Chalk(), Names::Constants::ODM());
    ENFORCE(id == Symbols::Chalk_ODM());

    id = enterClassSymbol(Loc::none(), Symbols::Chalk_ODM(), Names::Constants::DocumentDecoratorHelper());
    ENFORCE(id == Symbols::Chalk_ODM_DocumentDecoratorHelper());

    id = enterMethodSymbol(Loc::none(), Symbols::Sorbet_Private_StaticSingleton(), Names::sig());
    { enterMethodArgumentSymbol(Loc::none(), id, Names::arg0()); }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::arg1());
        arg.flags.isDefault = true;
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::blkArg());
        arg.flags.isBlock = true;
    }
    ENFORCE(id == Symbols::SorbetPrivateStaticSingleton_sig());

    id = enterClassSymbol(Loc::none(), Symbols::root(), Names::Constants::PackageRegistry());
    ENFORCE(id == Symbols::PackageRegistry());

    // PackageSpec is a class that can be subclassed.
    id = enterClassSymbol(Loc::none(), Symbols::root(), Names::Constants::PackageSpec());
    id.data(*this)->setIsModule(false);
    ENFORCE(id == Symbols::PackageSpec());

    id = id.data(*this)->singletonClass(*this);
    ENFORCE(id == Symbols::PackageSpecSingleton());

    id = enterMethodSymbol(Loc::none(), Symbols::PackageSpecSingleton(), Names::import());
    ENFORCE(id == Symbols::PackageSpec_import());
    {
        auto &importArg = enterMethodArgumentSymbol(Loc::none(), id, Names::arg0());
        // T.class_of(PackageSpec)
        importArg.type = make_type<ClassType>(Symbols::PackageSpecSingleton());
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::blkArg());
        arg.flags.isBlock = true;
    }

    id = enterMethodSymbol(Loc::none(), Symbols::PackageSpecSingleton(), Names::export_());
    ENFORCE(id == Symbols::PackageSpec_export());
    {
        enterMethodArgumentSymbol(Loc::none(), id, Names::arg0());
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::blkArg());
        arg.flags.isBlock = true;
    }

    id = enterMethodSymbol(Loc::none(), Symbols::PackageSpecSingleton(), Names::exportMethods());
    ENFORCE(id == Symbols::PackageSpec_export_methods());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), id, Names::arg0());
        arg.flags.isRepeated = true;
        auto &blkArg = enterMethodArgumentSymbol(Loc::none(), id, Names::blkArg());
        blkArg.flags.isBlock = true;
    }

    id = synthesizeClass(core::Names::Constants::Encoding());
    ENFORCE(id == Symbols::Encoding());

    // Root members
    Symbols::root().dataAllowingNone(*this)->members()[core::Names::Constants::NoSymbol()] = Symbols::noSymbol();
    Symbols::root().dataAllowingNone(*this)->members()[core::Names::Constants::Top()] = Symbols::top();
    Symbols::root().dataAllowingNone(*this)->members()[core::Names::Constants::Bottom()] = Symbols::bottom();

    // Synthesize <Magic>.<build-hash>(*vs : T.untyped) => Hash
    SymbolRef method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::buildHash());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.flags.isRepeated = true;
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::hashOfUntyped();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>#<build-keyword-args>(*vs : T.untyped) => Hash
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::buildKeywordArgs());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.flags.isRepeated = true;
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::hashOfUntyped();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<build-array>(*vs : T.untyped) => Array
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::buildArray());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.flags.isRepeated = true;
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::arrayOfUntyped();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }

    // Synthesize <Magic>.<build-range>(from: T.untyped, to: T.untyped) => Range
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::buildRange());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg1());
        arg.type = Types::untyped(*this, method);
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg2());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::rangeOfUntyped();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }

    // Synthesize <Magic>.<splat>(a: Array) => Untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::splat());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::arrayOfUntyped();
    }
    method.data(*this)->resultType = Types::untyped(*this, method);

    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }

    // Synthesize <Magic>.<defined>(*arg0: String) => Boolean
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::defined_p());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.flags.isRepeated = true;
        arg.type = Types::String();
    }
    method.data(*this)->resultType = Types::any(*this, Types::nilClass(), Types::String());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }

    // Synthesize <Magic>.<expandSplat>(arg0: T.untyped, arg1: Integer, arg2: Integer) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::expandSplat());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg1());
        arg.type = Types::Integer();
    }
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg2());
        arg.type = Types::Integer();
    }
    method.data(*this)->resultType = Types::untyped(*this, method);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<call-with-splat>(args: *T.untyped) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::callWithSplat());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
        arg.flags.isRepeated = true;
    }
    method.data(*this)->resultType = Types::untyped(*this, method);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<call-with-block>(args: *T.untyped) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::callWithBlock());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
        arg.flags.isRepeated = true;
    }
    method.data(*this)->resultType = Types::untyped(*this, method);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<call-with-splat-and-block>(args: *T.untyped) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::callWithSplatAndBlock());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
        arg.flags.isRepeated = true;
    }
    method.data(*this)->resultType = Types::untyped(*this, method);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<suggest-type>(arg: *T.untyped) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::suggestType());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::untyped(*this, method);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<self-new>(arg: *T.untyped) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::selfNew());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
        arg.flags.isRepeated = true;
    }
    method.data(*this)->resultType = Types::untyped(*this, method);
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<string-interpolate>(arg: *T.untyped) => String
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::stringInterpolate());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
        arg.flags.isRepeated = true;
    }
    method.data(*this)->resultType = Types::String();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<define-top-class-or-module>(arg: T.untyped) => Void
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::defineTopClassOrModule());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::void_();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<keep-for-cfg>(arg: T.untyped) => Void
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::keepForCfg());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::void_();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <Magic>.<retry>() => Void
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::retry());
    method.data(*this)->resultType = Types::void_();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }

    // Synthesize <Magic>.<blockBreak>(args: T.untyped) => T.untyped
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::blockBreak());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
        auto &argBlock = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        argBlock.flags.isBlock = true;
    }
    method.data(*this)->resultType = Types::untyped(*this, method);

    // Synthesize <Magic>.<getEncoding>() => Encoding
    method = enterMethodSymbol(Loc::none(), Symbols::MagicSingleton(), Names::getEncoding());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    method.data(*this)->resultType = core::make_type<core::ClassType>(core::Symbols::Encoding());

    // Synthesize <DeclBuilderForProcs>.<params>(args: T.untyped) => DeclBuilderForProcs
    method = enterMethodSymbol(Loc::none(), Symbols::DeclBuilderForProcsSingleton(), Names::params());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.flags.isDefault = true;
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::declBuilderForProcsSingletonClass();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <DeclBuilderForProcs>.<bind>(args: T.untyped) =>
    // DeclBuilderForProcs
    method = enterMethodSymbol(Loc::none(), Symbols::DeclBuilderForProcsSingleton(), Names::bind());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::declBuilderForProcsSingletonClass();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <DeclBuilderForProcs>.<returns>(args: T.untyped) => DeclBuilderForProcs
    method = enterMethodSymbol(Loc::none(), Symbols::DeclBuilderForProcsSingleton(), Names::returns());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::declBuilderForProcsSingletonClass();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Synthesize <DeclBuilderForProcs>.<type_parameters>(args: T.untyped) =>
    // DeclBuilderForProcs
    method = enterMethodSymbol(Loc::none(), Symbols::DeclBuilderForProcsSingleton(), Names::typeParameters());
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::arg0());
        arg.type = Types::untyped(*this, method);
    }
    method.data(*this)->resultType = Types::declBuilderForProcsSingletonClass();
    {
        auto &arg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
        arg.flags.isBlock = true;
    }
    // Some of these are Modules
    Symbols::StubModule().data(*this)->setIsModule(true);
    Symbols::T().data(*this)->setIsModule(true);
    Symbols::StubMixin().data(*this)->setIsModule(true);

    // Some of these are Classes
    Symbols::StubSuperClass().data(*this)->setIsModule(false);
    Symbols::StubSuperClass().data(*this)->setSuperClass(Symbols::Object());

    // Synthesize T::Utils
    id = enterClassSymbol(Loc::none(), Symbols::T(), core::Names::Constants::Utils());
    id.data(*this)->setIsModule(true);

    int reservedCount = 0;

    // Set the correct resultTypes for all synthesized classes
    // Does it in two passes since the singletonClass will go in the Symbols::root() members which will invalidate the
    // iterator
    vector<SymbolRef> needSingletons;
    for (auto &sym : classAndModules) {
        auto ref = sym.ref(*this);
        if (ref.exists()) {
            needSingletons.emplace_back(ref);
        }
    }
    for (auto sym : needSingletons) {
        sym.data(*this)->singletonClass(*this);
    }

    // This fills in all the way up to MAX_SYNTHETIC_CLASS_SYMBOLS
    ENFORCE(classAndModules.size() < Symbols::Proc0().classOrModuleIndex());
    while (classAndModules.size() < Symbols::Proc0().classOrModuleIndex()) {
        string name = absl::StrCat("<RESERVED_", reservedCount, ">");
        synthesizeClass(enterNameConstant(name));
        reservedCount++;
    }

    for (int arity = 0; arity <= Symbols::MAX_PROC_ARITY; ++arity) {
        string name = absl::StrCat("Proc", arity);
        auto id = synthesizeClass(enterNameConstant(name), Symbols::Proc().classOrModuleIndex());
        ENFORCE(id == Symbols::Proc(arity), "Proc creation failed for arity: {} got: {} expected: {}", arity,
                id.classOrModuleIndex(), Symbols::Proc(arity).classOrModuleIndex());
        id.data(*this)->singletonClass(*this);
    }

    ENFORCE(classAndModules.size() == Symbols::last_synthetic_class_sym().classOrModuleIndex() + 1,
            "Too many synthetic class symbols? have: {} expected: {}", classAndModules.size(),
            Symbols::last_synthetic_class_sym().classOrModuleIndex() + 1);

    ENFORCE(methods.size() == Symbols::MAX_SYNTHETIC_METHOD_SYMBOLS,
            "Too many synthetic method symbols? have: {} expected: {}", methods.size(),
            Symbols::MAX_SYNTHETIC_METHOD_SYMBOLS);
    ENFORCE(fields.size() == Symbols::MAX_SYNTHETIC_FIELD_SYMBOLS,
            "Too many synthetic field symbols? have: {} expected: {}", fields.size(),
            Symbols::MAX_SYNTHETIC_FIELD_SYMBOLS);
    ENFORCE(typeMembers.size() == Symbols::MAX_SYNTHETIC_TYPEMEMBER_SYMBOLS,
            "Too many synthetic typeMember symbols? have: {} expected: {}", typeMembers.size(),
            Symbols::MAX_SYNTHETIC_TYPEMEMBER_SYMBOLS);
    ENFORCE(typeArguments.size() == Symbols::MAX_SYNTHETIC_TYPEARGUMENT_SYMBOLS,
            "Too many synthetic typeArgument symbols? have: {} expected: {}", typeArguments.size(),
            Symbols::MAX_SYNTHETIC_TYPEARGUMENT_SYMBOLS);

    installIntrinsics();

    Symbols::top().data(*this)->resultType = Types::top();
    Symbols::bottom().data(*this)->resultType = Types::bottom();
    Symbols::NilClass().data(*this)->resultType = Types::nilClass();
    Symbols::untyped().data(*this)->resultType = Types::untypedUntracked();
    Symbols::FalseClass().data(*this)->resultType = Types::falseClass();
    Symbols::TrueClass().data(*this)->resultType = Types::trueClass();
    Symbols::Integer().data(*this)->resultType = Types::Integer();
    Symbols::String().data(*this)->resultType = Types::String();
    Symbols::Symbol().data(*this)->resultType = Types::Symbol();
    Symbols::Float().data(*this)->resultType = Types::Float();
    Symbols::Object().data(*this)->resultType = Types::Object();
    Symbols::Class().data(*this)->resultType = Types::classClass();

    // First file is used to indicate absence of a file
    files.emplace_back();
    freezeNameTable();
    freezeSymbolTable();
    freezeFileTable();
    sanityCheck();
}

void GlobalState::installIntrinsics() {
    for (auto &entry : intrinsicMethods) {
        SymbolRef symbol;
        switch (entry.singleton) {
            case Intrinsic::Kind::Instance:
                symbol = entry.symbol;
                break;
            case Intrinsic::Kind::Singleton:
                symbol = entry.symbol.data(*this)->singletonClass(*this);
                break;
        }
        auto countBefore = methodsUsed();
        SymbolRef method = enterMethodSymbol(Loc::none(), symbol, entry.method);
        method.data(*this)->intrinsic = entry.impl;
        if (countBefore != methodsUsed()) {
            auto &blkArg = enterMethodArgumentSymbol(Loc::none(), method, Names::blkArg());
            blkArg.flags.isBlock = true;
        }
    }
}

// https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
u4 nextPowerOfTwo(u4 v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

void GlobalState::preallocateTables(u4 classAndModulesSize, u4 methodsSize, u4 fieldsSize, u4 typeArgumentsSize,
                                    u4 typeMembersSize, u4 nameSize) {
    u4 classAndModulesSizeScaled = nextPowerOfTwo(classAndModulesSize);
    u4 methodsSizeScaled = nextPowerOfTwo(methodsSize);
    u4 fieldsSizeScaled = nextPowerOfTwo(fieldsSize);
    u4 typeArgumentsSizeScaled = nextPowerOfTwo(typeArgumentsSize);
    u4 typeMembersSizeScaled = nextPowerOfTwo(typeMembersSize);
    u4 nameSizeScaled = nextPowerOfTwo(nameSize);

    // Note: reserve is a no-op if size is < current capacity.
    classAndModules.reserve(classAndModulesSizeScaled);
    methods.reserve(methodsSizeScaled);
    fields.reserve(fieldsSizeScaled);
    typeArguments.reserve(typeArgumentsSizeScaled);
    typeMembers.reserve(typeMembersSizeScaled);
    expandNames(nameSizeScaled);
    sanityCheck();

    trace(fmt::format("Preallocated symbol and name tables. classAndModules={} methods={} fields={} typeArguments={} "
                      "typeMembers={} names={}",
                      classAndModules.capacity(), methods.capacity(), fields.capacity(), typeArguments.capacity(),
                      typeMembers.capacity(), names.capacity()));
}

constexpr decltype(GlobalState::STRINGS_PAGE_SIZE) GlobalState::STRINGS_PAGE_SIZE;

SymbolRef GlobalState::lookupMethodSymbolWithHash(SymbolRef owner, NameRef name, const vector<u4> &methodHash) const {
    ENFORCE(owner.exists(), "looking up symbol from non-existing owner");
    ENFORCE(name.exists(), "looking up symbol with non-existing name");
    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_lookup_by_name", ownerScope->members().size());

    NameRef lookupName = name;
    u4 unique = 1;
    auto res = ownerScope->members().find(lookupName);
    while (res != ownerScope->members().end()) {
        ENFORCE(res->second.exists());
        auto resData = res->second.data(*this);
        if ((resData->flags & Symbol::Flags::METHOD) == Symbol::Flags::METHOD &&
            (resData->methodArgumentHash(*this) == methodHash ||
             (resData->intrinsic != nullptr && !resData->hasSig()))) {
            return res->second;
        }
        lookupName = lookupNameUnique(UniqueNameKind::MangleRename, name, unique);
        if (!lookupName.exists()) {
            break;
        }
        res = ownerScope->members().find(lookupName);
        unique++;
    }
    return Symbols::noSymbol();
}

// look up a symbol whose flags match the desired flags. This might look through mangled names to discover one whose
// flags match. If no sych symbol exists, then it will return noSymbol.
SymbolRef GlobalState::lookupSymbolWithFlags(SymbolRef owner, NameRef name, u4 flags) const {
    ENFORCE(owner.exists(), "looking up symbol from non-existing owner");
    ENFORCE(name.exists(), "looking up symbol with non-existing name");
    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_lookup_by_name", ownerScope->members().size());

    NameRef lookupName = name;
    u4 unique = 1;
    auto res = ownerScope->members().find(lookupName);
    while (res != ownerScope->members().end()) {
        ENFORCE(res->second.exists());
        if ((res->second.data(*this)->flags & flags) == flags) {
            return res->second;
        }
        lookupName = lookupNameUnique(UniqueNameKind::MangleRename, name, unique);
        if (!lookupName.exists()) {
            break;
        }
        res = ownerScope->members().find(lookupName);
        unique++;
    }
    return Symbols::noSymbol();
}

SymbolRef GlobalState::findRenamedSymbol(SymbolRef owner, SymbolRef sym) const {
    // This method works by knowing how to replicate the logic of renaming in order to find whatever
    // the previous name was: for `x$n` where `n` is larger than 2, it'll be `x$(n-1)`, for bare `x`,
    // it'll be whatever the largest `x$n` that exists is, if any; otherwise, there will be none.
    ENFORCE(sym.exists(), "lookup up previous name of non-existing symbol");
    NameRef name = sym.data(*this)->name;
    NameData nameData = name.data(*this);
    SymbolData ownerScope = owner.dataAllowingNone(*this);

    if (nameData->kind == NameKind::UNIQUE) {
        if (nameData->unique.uniqueNameKind != UniqueNameKind::MangleRename) {
            return Symbols::noSymbol();
        }
        if (nameData->unique.num == 1) {
            return Symbols::noSymbol();
        } else {
            ENFORCE(nameData->unique.num > 1);
            auto nm =
                lookupNameUnique(UniqueNameKind::MangleRename, nameData->unique.original, nameData->unique.num - 1);
            if (!nm.exists()) {
                return Symbols::noSymbol();
            }
            auto res = ownerScope->members()[nm];
            ENFORCE(res.exists());
            return res;
        }
    } else {
        u4 unique = 1;
        NameRef lookupName = lookupNameUnique(UniqueNameKind::MangleRename, name, unique);
        auto res = ownerScope->members().find(lookupName);
        while (res != ownerScope->members().end()) {
            ENFORCE(res->second.exists());
            unique++;
            lookupName = lookupNameUnique(UniqueNameKind::MangleRename, name, unique);
            if (!lookupName.exists()) {
                return res->second;
            }
            res = ownerScope->members().find(lookupName);
        }
        return Symbols::noSymbol();
    }
}

SymbolRef GlobalState::enterClassSymbol(Loc loc, SymbolRef owner, NameRef name) {
    ENFORCE_NO_TIMER(!owner.exists() || // used when entering entirely syntehtic classes
                     owner.data(*this)->isClassOrModule());
    ENFORCE_NO_TIMER(name.data(*this)->isClassName(*this));
    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_enter_by_name", ownerScope->members().size());

    auto flags = Symbol::Flags::CLASS_OR_MODULE;
    auto &store = ownerScope->members()[name];
    if (store.exists()) {
        ENFORCE_NO_TIMER((store.data(*this)->flags & flags) == flags, "existing symbol has wrong flags");
        counterInc("symbols.hit");
        return store;
    }

    ENFORCE_NO_TIMER(!symbolTableFrozen);
    auto ret = SymbolRef(this, SymbolRef::Kind::ClassOrModule, classAndModules.size());
    store = ret; // DO NOT MOVE this assignment down. emplace_back on classAndModules invalidates `store`
    classAndModules.emplace_back();
    SymbolData data = ret.dataAllowingNone(*this);
    data->name = name;
    data->flags = flags;
    data->owner = owner;
    data->addLoc(*this, loc);
    DEBUG_ONLY(categoryCounterInc("symbols", "class"));
    wasModified_ = true;

    return ret;
}

SymbolRef GlobalState::enterTypeMember(Loc loc, SymbolRef owner, NameRef name, Variance variance) {
    u4 flags;
    ENFORCE(owner.data(*this)->isClassOrModule());
    ENFORCE(name.exists());
    if (variance == Variance::Invariant) {
        flags = Symbol::Flags::TYPE_INVARIANT;
    } else if (variance == Variance::CoVariant) {
        flags = Symbol::Flags::TYPE_COVARIANT;
    } else if (variance == Variance::ContraVariant) {
        flags = Symbol::Flags::TYPE_CONTRAVARIANT;
    } else {
        Exception::notImplemented();
    }

    flags = flags | Symbol::Flags::TYPE_MEMBER;

    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_enter_by_name", ownerScope->members().size());

    auto &store = ownerScope->members()[name];
    if (store.exists()) {
        ENFORCE((store.data(*this)->flags & flags) == flags, "existing symbol has wrong flags");
        counterInc("symbols.hit");
        return store;
    }

    ENFORCE(!symbolTableFrozen);
    auto result = SymbolRef(this, SymbolRef::Kind::TypeMember, typeMembers.size());
    store = result; // DO NOT MOVE this assignment down. emplace_back on typeMembers invalidates `store`
    typeMembers.emplace_back();

    SymbolData data = result.dataAllowingNone(*this);
    data->name = name;
    data->flags = flags;
    data->owner = owner;
    data->addLoc(*this, loc);
    DEBUG_ONLY(categoryCounterInc("symbols", "type_member"));
    wasModified_ = true;

    auto &members = owner.data(*this)->typeMembers();
    if (!absl::c_linear_search(members, result)) {
        members.emplace_back(result);
    }
    return result;
}

SymbolRef GlobalState::enterTypeArgument(Loc loc, SymbolRef owner, NameRef name, Variance variance) {
    ENFORCE(owner.exists());
    ENFORCE(name.exists());
    u4 flags;
    if (variance == Variance::Invariant) {
        flags = Symbol::Flags::TYPE_INVARIANT;
    } else if (variance == Variance::CoVariant) {
        flags = Symbol::Flags::TYPE_COVARIANT;
    } else if (variance == Variance::ContraVariant) {
        flags = Symbol::Flags::TYPE_CONTRAVARIANT;
    } else {
        Exception::notImplemented();
    }

    flags = flags | Symbol::Flags::TYPE_ARGUMENT;

    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_enter_by_name", ownerScope->members().size());

    auto &store = ownerScope->members()[name];
    if (store.exists()) {
        ENFORCE((store.data(*this)->flags & flags) == flags, "existing symbol has wrong flags");
        counterInc("symbols.hit");
        return store;
    }

    ENFORCE(!symbolTableFrozen);
    auto result = SymbolRef(this, SymbolRef::Kind::TypeArgument, typeArguments.size());
    store = result; // DO NOT MOVE this assignment down. emplace_back on typeArguments invalidates `store`
    typeArguments.emplace_back();

    SymbolData data = result.dataAllowingNone(*this);
    data->name = name;
    data->flags = flags;
    data->owner = owner;
    data->addLoc(*this, loc);
    DEBUG_ONLY(categoryCounterInc("symbols", "type_argument"));
    wasModified_ = true;

    owner.data(*this)->typeArguments().emplace_back(result);
    return result;
}

SymbolRef GlobalState::enterMethodSymbol(Loc loc, SymbolRef owner, NameRef name) {
    bool isBlock =
        name.data(*this)->kind == NameKind::UNIQUE && name.data(*this)->unique.original == Names::blockTemp();
    ENFORCE(isBlock || owner.data(*this)->isClassOrModule(), "entering method symbol into not-a-class");

    auto flags = Symbol::Flags::METHOD;

    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_enter_by_name", ownerScope->members().size());

    auto &store = ownerScope->members()[name];
    if (store.exists()) {
        ENFORCE((store.data(*this)->flags & flags) == flags, "existing symbol has wrong flags");
        counterInc("symbols.hit");
        return store;
    }

    ENFORCE(!symbolTableFrozen);

    auto result = SymbolRef(this, SymbolRef::Kind::Method, methods.size());
    store = result; // DO NOT MOVE this assignment down. emplace_back on methods invalidates `store`
    methods.emplace_back();

    SymbolData data = result.dataAllowingNone(*this);
    data->name = name;
    data->flags = flags;
    data->owner = owner;
    data->addLoc(*this, loc);
    DEBUG_ONLY(categoryCounterInc("symbols", "method"));
    wasModified_ = true;

    return result;
}

SymbolRef GlobalState::enterNewMethodOverload(Loc sigLoc, SymbolRef original, core::NameRef originalName, u4 num,
                                              const vector<int> &argsToKeep) {
    NameRef name = num == 0 ? originalName : freshNameUnique(UniqueNameKind::Overload, originalName, num);
    core::Loc loc = num == 0 ? original.data(*this)->loc()
                             : sigLoc; // use original Loc for main overload so that we get right jump-to-def for it.
    auto owner = original.data(*this)->owner;
    SymbolRef res = enterMethodSymbol(loc, owner, name);
    ENFORCE(res != original);
    if (res.data(*this)->arguments().size() != original.data(*this)->arguments().size()) {
        ENFORCE(res.data(*this)->arguments().empty());
        res.data(*this)->arguments().reserve(original.data(*this)->arguments().size());
        const auto &originalArguments = original.data(*this)->arguments();
        int i = -1;
        for (auto &arg : originalArguments) {
            i += 1;
            Loc loc = arg.loc;
            if (!absl::c_linear_search(argsToKeep, i)) {
                if (arg.flags.isBlock) {
                    loc = Loc::none();
                } else {
                    continue;
                }
            }
            NameRef nm = arg.name;
            auto &newArg = enterMethodArgumentSymbol(loc, res, nm);
            newArg = arg.deepCopy();
            newArg.loc = loc;
        }
    }
    return res;
}

SymbolRef GlobalState::enterFieldSymbol(Loc loc, SymbolRef owner, NameRef name) {
    ENFORCE(owner.data(*this)->isClassOrModule(), "entering field symbol into not-a-class");
    ENFORCE(name.exists());

    auto flags = Symbol::Flags::FIELD;
    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_enter_by_name", ownerScope->members().size());

    auto &store = ownerScope->members()[name];
    if (store.exists()) {
        ENFORCE((store.data(*this)->flags & flags) == flags, "existing symbol has wrong flags");
        counterInc("symbols.hit");
        return store;
    }

    ENFORCE(!symbolTableFrozen);

    auto result = SymbolRef(this, SymbolRef::Kind::Field, fields.size());
    store = result; // DO NOT MOVE this assignment down. emplace_back on fields invalidates `store`
    fields.emplace_back();

    SymbolData data = result.dataAllowingNone(*this);
    data->name = name;
    data->flags = flags;
    data->owner = owner;
    data->addLoc(*this, loc);

    DEBUG_ONLY(categoryCounterInc("symbols", "field"));
    wasModified_ = true;

    return result;
}

SymbolRef GlobalState::enterStaticFieldSymbol(Loc loc, SymbolRef owner, NameRef name) {
    ENFORCE(owner.data(*this)->isClassOrModule());
    ENFORCE(name.exists());

    SymbolData ownerScope = owner.dataAllowingNone(*this);
    histogramInc("symbol_enter_by_name", ownerScope->members().size());

    auto flags = Symbol::Flags::STATIC_FIELD;
    auto &store = ownerScope->members()[name];
    if (store.exists()) {
        ENFORCE((store.data(*this)->flags & flags) == flags, "existing symbol has wrong flags");
        counterInc("symbols.hit");
        return store;
    }

    ENFORCE(!symbolTableFrozen);

    auto ret = SymbolRef(this, SymbolRef::Kind::Field, fields.size());
    store = ret; // DO NOT MOVE this assignment down. emplace_back on fields invalidates `store`
    fields.emplace_back();

    SymbolData data = ret.dataAllowingNone(*this);
    data->name = name;
    data->flags = flags;
    data->owner = owner;
    data->addLoc(*this, loc);

    DEBUG_ONLY(categoryCounterInc("symbols", "static_field"));
    wasModified_ = true;

    return ret;
}

ArgInfo &GlobalState::enterMethodArgumentSymbol(Loc loc, SymbolRef owner, NameRef name) {
    ENFORCE(owner.exists(), "entering symbol in to non-existing owner");
    ENFORCE(owner.data(*this)->isMethod(), "entering method argument symbol into not-a-method");
    ENFORCE(name.exists(), "entering symbol with non-existing name");
    SymbolData ownerScope = owner.dataAllowingNone(*this);

    for (auto &arg : ownerScope->arguments()) {
        if (arg.name == name) {
            return arg;
        }
    }
    auto &store = ownerScope->arguments().emplace_back();

    ENFORCE(!symbolTableFrozen);

    store.name = name;
    store.loc = loc;
    DEBUG_ONLY(categoryCounterInc("symbols", "argument"););

    wasModified_ = true;
    return store;
}

string_view GlobalState::enterString(string_view nm) {
    DEBUG_ONLY(if (ensureCleanStrings) {
        if (nm != "<" && nm != "<<" && nm != "<=" && nm != "<=>" && nm != ">" && nm != ">>" && nm != ">=") {
            ENFORCE(nm.find("<") == string::npos);
            ENFORCE(nm.find(">") == string::npos);
        }
    });
    char *from = nullptr;
    if (nm.size() > GlobalState::STRINGS_PAGE_SIZE) {
        auto &inserted = strings.emplace_back(make_unique<vector<char>>(nm.size()));
        from = inserted->data();
        if (strings.size() > 1) {
            // last page wasn't full, keep it in the end
            swap(*(strings.end() - 1), *(strings.end() - 2));
        }
    } else {
        if (stringsLastPageUsed + nm.size() > GlobalState::STRINGS_PAGE_SIZE) {
            strings.emplace_back(make_unique<vector<char>>(GlobalState::STRINGS_PAGE_SIZE));
            // printf("Wasted %i space\n", STRINGS_PAGE_SIZE - stringsLastPageUsed);
            stringsLastPageUsed = 0;
        }
        from = strings.back()->data() + stringsLastPageUsed;
    }

    counterInc("strings");
    memcpy(from, nm.data(), nm.size());
    stringsLastPageUsed += nm.size();
    return string_view(from, nm.size());
}

NameRef GlobalState::lookupNameUTF8(string_view nm) const {
    const auto hs = _hash(nm);
    unsigned int hashTableSize = namesByHash.size();
    unsigned int mask = hashTableSize - 1;
    auto bucketId = hs & mask;
    unsigned int probeCount = 1;

    while (namesByHash[bucketId].second != 0u) {
        auto &bucket = namesByHash[bucketId];
        if (bucket.first == hs) {
            auto nameId = bucket.second;
            auto &nm2 = names[nameId];
            if (nm2.kind == NameKind::UTF8 && nm2.raw.utf8 == nm) {
                counterInc("names.utf8.hit");
                return nm2.ref(*this);
            } else {
                counterInc("names.hash_collision.utf8");
            }
        }
        bucketId = (bucketId + probeCount) & mask;
        probeCount++;
    }

    return core::NameRef::noName();
}

NameRef GlobalState::enterNameUTF8(string_view nm) {
    const auto hs = _hash(nm);
    unsigned int hashTableSize = namesByHash.size();
    unsigned int mask = hashTableSize - 1;
    auto bucketId = hs & mask;
    unsigned int probeCount = 1;

    while (namesByHash[bucketId].second != 0u) {
        auto &bucket = namesByHash[bucketId];
        if (bucket.first == hs) {
            auto nameId = bucket.second;
            auto &nm2 = names[nameId];
            if (nm2.kind == NameKind::UTF8 && nm2.raw.utf8 == nm) {
                counterInc("names.utf8.hit");
                return nm2.ref(*this);
            } else {
                counterInc("names.hash_collision.utf8");
            }
        }
        bucketId = (bucketId + probeCount) & mask;
        probeCount++;
    }
    ENFORCE(!nameTableFrozen);

    ENFORCE(probeCount != hashTableSize, "Full table?");

    if (names.size() == names.capacity()) {
        expandNames(names.capacity() * 2);
        hashTableSize = namesByHash.size();
        mask = hashTableSize - 1;
        bucketId = hs & mask; // look for place in the new size
        probeCount = 1;
        while (namesByHash[bucketId].second != 0) {
            bucketId = (bucketId + probeCount) & mask;
            probeCount++;
        }
    }

    auto idx = names.size();
    auto &bucket = namesByHash[bucketId];
    bucket.first = hs;
    bucket.second = idx;
    names.emplace_back();

    names[idx].kind = NameKind::UTF8;
    names[idx].raw.utf8 = enterString(nm);
    ENFORCE(names[idx].hash(*this) == hs);
    categoryCounterInc("names", "utf8");

    wasModified_ = true;
    return NameRef(*this, idx);
}

NameRef GlobalState::enterNameConstant(NameRef original) {
    ENFORCE(original.exists(), "making a constant name over non-existing name");
    ENFORCE(original.data(*this)->kind == NameKind::UTF8 ||
                (original.data(*this)->kind == NameKind::UNIQUE &&
                 (original.data(*this)->unique.uniqueNameKind == UniqueNameKind::ResolverMissingClass ||
                  original.data(*this)->unique.uniqueNameKind == UniqueNameKind::TEnum)),
            "making a constant name over wrong name kind");

    const auto hs = _hash_mix_constant(NameKind::CONSTANT, original.id());
    unsigned int hashTableSize = namesByHash.size();
    unsigned int mask = hashTableSize - 1;
    auto bucketId = hs & mask;
    unsigned int probeCount = 1;

    while (namesByHash[bucketId].second != 0 && probeCount < hashTableSize) {
        auto &bucket = namesByHash[bucketId];
        if (bucket.first == hs) {
            auto &nm2 = names[bucket.second];
            if (nm2.kind == NameKind::CONSTANT && nm2.cnst.original == original) {
                counterInc("names.constant.hit");
                return nm2.ref(*this);
            } else {
                counterInc("names.hash_collision.constant");
            }
        }
        bucketId = (bucketId + probeCount) & mask;
        probeCount++;
    }
    if (probeCount == hashTableSize) {
        Exception::raise("Full table?");
    }
    ENFORCE(!nameTableFrozen);

    if (names.size() == names.capacity()) {
        expandNames(names.capacity() * 2);
        hashTableSize = namesByHash.size();
        mask = hashTableSize - 1;

        bucketId = hs & mask; // look for place in the new size
        probeCount = 1;
        while (namesByHash[bucketId].second != 0) {
            bucketId = (bucketId + probeCount) & mask;
            probeCount++;
        }
    }

    auto &bucket = namesByHash[bucketId];
    bucket.first = hs;
    bucket.second = names.size();

    auto idx = names.size();
    names.emplace_back();

    names[idx].kind = NameKind::CONSTANT;
    names[idx].cnst.original = original;
    ENFORCE(names[idx].hash(*this) == hs);
    wasModified_ = true;
    categoryCounterInc("names", "constant");
    return NameRef(*this, idx);
}

NameRef GlobalState::enterNameConstant(string_view original) {
    return enterNameConstant(enterNameUTF8(original));
}

NameRef GlobalState::lookupNameConstant(NameRef original) const {
    if (!original.exists()) {
        return core::NameRef::noName();
    }
    ENFORCE(original.data(*this)->kind == NameKind::UTF8 ||
                (original.data(*this)->kind == NameKind::UNIQUE &&
                 (original.data(*this)->unique.uniqueNameKind == UniqueNameKind::ResolverMissingClass ||
                  original.data(*this)->unique.uniqueNameKind == UniqueNameKind::TEnum)),
            "looking up a constant name over wrong name kind");

    const auto hs = _hash_mix_constant(NameKind::CONSTANT, original.id());
    unsigned int hashTableSize = namesByHash.size();
    unsigned int mask = hashTableSize - 1;
    auto bucketId = hs & mask;
    unsigned int probeCount = 1;

    while (namesByHash[bucketId].second != 0 && probeCount < hashTableSize) {
        auto &bucket = namesByHash[bucketId];
        if (bucket.first == hs) {
            auto &nm2 = names[bucket.second];
            if (nm2.kind == NameKind::CONSTANT && nm2.cnst.original == original) {
                counterInc("names.constant.hit");
                return nm2.ref(*this);
            } else {
                counterInc("names.hash_collision.constant");
            }
        }
        bucketId = (bucketId + probeCount) & mask;
        probeCount++;
    }

    return core::NameRef::noName();
}

NameRef GlobalState::lookupNameConstant(string_view original) const {
    auto utf8 = lookupNameUTF8(original);
    if (!utf8.exists()) {
        return core::NameRef::noName();
    }
    return lookupNameConstant(utf8);
}

void moveNames(pair<unsigned int, unsigned int> *from, pair<unsigned int, unsigned int> *to, unsigned int szFrom,
               unsigned int szTo) {
    // printf("\nResizing name hash table from %u to %u\n", szFrom, szTo);
    ENFORCE((szTo & (szTo - 1)) == 0, "name hash table size corruption");
    ENFORCE((szFrom & (szFrom - 1)) == 0, "name hash table size corruption");
    unsigned int mask = szTo - 1;
    for (unsigned int orig = 0; orig < szFrom; orig++) {
        if (from[orig].second != 0u) {
            auto hs = from[orig].first;
            unsigned int probe = 1;
            auto bucketId = hs & mask;
            while (to[bucketId].second != 0) {
                bucketId = (bucketId + probe) & mask;
                probe++;
            }
            to[bucketId] = from[orig];
        }
    }
}

void GlobalState::expandNames(u4 newSize) {
    sanityCheck();
    if (newSize > names.capacity()) {
        names.reserve(newSize);
        vector<pair<unsigned int, unsigned int>> new_namesByHash(newSize * 2);
        moveNames(namesByHash.data(), new_namesByHash.data(), namesByHash.size(), new_namesByHash.capacity());
        namesByHash.swap(new_namesByHash);
    }
}

NameRef GlobalState::lookupNameUnique(UniqueNameKind uniqueNameKind, NameRef original, u4 num) const {
    ENFORCE(num > 0, "num == 0, name overflow");
    const auto hs = _hash_mix_unique((u2)uniqueNameKind, NameKind::UNIQUE, num, original.id());
    unsigned int hashTableSize = namesByHash.size();
    unsigned int mask = hashTableSize - 1;
    auto bucketId = hs & mask;
    unsigned int probeCount = 1;

    while (namesByHash[bucketId].second != 0 && probeCount < hashTableSize) {
        auto &bucket = namesByHash[bucketId];
        if (bucket.first == hs) {
            auto &nm2 = names[bucket.second];
            if (nm2.kind == NameKind::UNIQUE && nm2.unique.uniqueNameKind == uniqueNameKind && nm2.unique.num == num &&
                nm2.unique.original == original) {
                counterInc("names.unique.hit");
                return nm2.ref(*this);
            } else {
                counterInc("names.hash_collision.unique");
            }
        }
        bucketId = (bucketId + probeCount) & mask;
        probeCount++;
    }
    return core::NameRef::noName();
}

NameRef GlobalState::freshNameUnique(UniqueNameKind uniqueNameKind, NameRef original, u4 num) {
    ENFORCE(num > 0, "num == 0, name overflow");
    const auto hs = _hash_mix_unique((u2)uniqueNameKind, NameKind::UNIQUE, num, original.id());
    unsigned int hashTableSize = namesByHash.size();
    unsigned int mask = hashTableSize - 1;
    auto bucketId = hs & mask;
    unsigned int probeCount = 1;

    while (namesByHash[bucketId].second != 0 && probeCount < hashTableSize) {
        auto &bucket = namesByHash[bucketId];
        if (bucket.first == hs) {
            auto &nm2 = names[bucket.second];
            if (nm2.kind == NameKind::UNIQUE && nm2.unique.uniqueNameKind == uniqueNameKind && nm2.unique.num == num &&
                nm2.unique.original == original) {
                counterInc("names.unique.hit");
                return nm2.ref(*this);
            } else {
                counterInc("names.hash_collision.unique");
            }
        }
        bucketId = (bucketId + probeCount) & mask;
        probeCount++;
    }
    if (probeCount == hashTableSize) {
        Exception::raise("Full table?");
    }
    ENFORCE(!nameTableFrozen);

    if (names.size() == names.capacity()) {
        expandNames(names.capacity() * 2);
        hashTableSize = namesByHash.size();
        mask = hashTableSize - 1;

        bucketId = hs & mask; // look for place in the new size
        probeCount = 1;
        while (namesByHash[bucketId].second != 0) {
            bucketId = (bucketId + probeCount) & mask;
            probeCount++;
        }
    }

    auto &bucket = namesByHash[bucketId];
    bucket.first = hs;
    bucket.second = names.size();

    auto idx = names.size();
    names.emplace_back();

    names[idx].kind = NameKind::UNIQUE;
    names[idx].unique.num = num;
    names[idx].unique.uniqueNameKind = uniqueNameKind;
    names[idx].unique.original = original;
    ENFORCE(names[idx].hash(*this) == hs);
    wasModified_ = true;
    categoryCounterInc("names", "unique");
    return NameRef(*this, idx);
}

FileRef GlobalState::enterFile(const shared_ptr<File> &file) {
    ENFORCE(!fileTableFrozen);

    DEBUG_ONLY(for (auto &f
                    : this->files) {
        if (f) {
            if (f->path() == file->path()) {
                Exception::raise("should never happen");
            }
        }
    })

    files.emplace_back(file);
    auto ret = FileRef(filesUsed() - 1);
    fileRefByPath[string(file->path())] = ret;
    return ret;
}

FileRef GlobalState::enterFile(string_view path, string_view source) {
    return GlobalState::enterFile(
        make_shared<File>(string(path.begin(), path.end()), string(source.begin(), source.end()), File::Type::Normal));
}

FileRef GlobalState::enterNewFileAt(const shared_ptr<File> &file, FileRef id) {
    ENFORCE(!fileTableFrozen);
    ENFORCE(id.id() < this->files.size());
    ENFORCE(this->files[id.id()]->sourceType == File::Type::NotYetRead);
    ENFORCE(this->files[id.id()]->path() == file->path());

    // was a tombstone before.
    this->files[id.id()] = file;
    FileRef result(id);
    return result;
}

FileRef GlobalState::reserveFileRef(string path) {
    return GlobalState::enterFile(make_shared<File>(move(path), "", File::Type::NotYetRead));
}

void GlobalState::mangleRenameSymbol(SymbolRef what, NameRef origName) {
    auto whatData = what.data(*this);
    auto owner = whatData->owner;
    auto ownerData = owner.data(*this);
    auto &ownerMembers = ownerData->members();
    auto fnd = ownerMembers.find(origName);
    ENFORCE(fnd != ownerMembers.end());
    ENFORCE(fnd->second == what);
    ENFORCE(whatData->name == origName);
    u4 collisionCount = 1;
    NameRef name;
    do {
        name = freshNameUnique(UniqueNameKind::MangleRename, origName, collisionCount++);
    } while (ownerData->findMember(*this, name).exists());
    ownerMembers.erase(fnd);
    ownerMembers[name] = what;
    whatData->name = name;
    if (whatData->isClassOrModule()) {
        auto singleton = whatData->lookupSingletonClass(*this);
        if (singleton.exists()) {
            mangleRenameSymbol(singleton, singleton.data(*this)->name);
        }
    }
}

unsigned int GlobalState::classAndModulesUsed() const {
    return classAndModules.size();
}

unsigned int GlobalState::methodsUsed() const {
    return methods.size();
}

unsigned int GlobalState::fieldsUsed() const {
    return fields.size();
}

unsigned int GlobalState::typeArgumentsUsed() const {
    return typeArguments.size();
}

unsigned int GlobalState::typeMembersUsed() const {
    return typeMembers.size();
}

unsigned int GlobalState::filesUsed() const {
    return files.size();
}

unsigned int GlobalState::namesUsed() const {
    return names.size();
}

unsigned int GlobalState::symbolsUsedTotal() const {
    return classAndModulesUsed() + methodsUsed() + fieldsUsed() + typeArgumentsUsed() + typeMembersUsed();
}

string GlobalState::toStringWithOptions(bool showFull, bool showRaw) const {
    return Symbols::root().data(*this)->toStringWithOptions(*this, 0, showFull, showRaw);
}

void GlobalState::sanityCheck() const {
    if (!debug_mode) {
        return;
    }
    if (fuzz_mode) {
        // it's very slow to check this and it didn't find bugs
        return;
    }

    Timer timeit(tracer(), "GlobalState::sanityCheck");
    ENFORCE(!names.empty(), "empty name table size");
    ENFORCE(!strings.empty(), "empty string table size");
    ENFORCE(!namesByHash.empty(), "empty name hash table size");
    ENFORCE((namesByHash.size() & (namesByHash.size() - 1)) == 0, "name hash table size is not a power of two");
    ENFORCE(names.capacity() * 2 == namesByHash.capacity(),
            "name table and hash name table sizes out of sync names.capacity={} namesByHash.capacity={}",
            names.capacity(), namesByHash.capacity());
    ENFORCE(namesByHash.size() == namesByHash.capacity(), "hash name table not at full capacity");
    int i = -1;
    for (auto &nm : names) {
        i++;
        if (i != 0) {
            nm.sanityCheck(*this);
        }
    }

    i = -1;
    for (auto &sym : classAndModules) {
        i++;
        if (i != 0) {
            sym.sanityCheck(*this);
        }
    }
    for (auto &sym : methods) {
        sym.sanityCheck(*this);
    }
    for (auto &sym : fields) {
        sym.sanityCheck(*this);
    }
    for (auto &sym : typeArguments) {
        sym.sanityCheck(*this);
    }
    for (auto &sym : typeMembers) {
        sym.sanityCheck(*this);
    }
    for (auto &ent : namesByHash) {
        if (ent.second == 0) {
            continue;
        }
        const Name &nm = names[ent.second];
        ENFORCE_NO_TIMER(ent.first == nm.hash(*this), "name hash table corruption");
    }
}

bool GlobalState::freezeNameTable() {
    bool old = this->nameTableFrozen;
    this->nameTableFrozen = true;
    return old;
}

bool GlobalState::freezeFileTable() {
    bool old = this->fileTableFrozen;
    this->fileTableFrozen = true;
    return old;
}

bool GlobalState::freezeSymbolTable() {
    bool old = this->symbolTableFrozen;
    this->symbolTableFrozen = true;
    return old;
}

bool GlobalState::unfreezeNameTable() {
    bool old = this->nameTableFrozen;
    this->nameTableFrozen = false;
    return old;
}

bool GlobalState::unfreezeFileTable() {
    bool old = this->fileTableFrozen;
    this->fileTableFrozen = false;
    return old;
}

bool GlobalState::unfreezeSymbolTable() {
    bool old = this->symbolTableFrozen;
    this->symbolTableFrozen = false;
    return old;
}

unique_ptr<GlobalState> GlobalState::deepCopy(bool keepId) const {
    Timer timeit(tracer(), "GlobalState::deepCopy", this->creation);
    this->sanityCheck();
    auto result = make_unique<GlobalState>(this->errorQueue, this->epochManager);

    result->silenceErrors = this->silenceErrors;
    result->autocorrect = this->autocorrect;
    result->ensureCleanStrings = this->ensureCleanStrings;
    result->runningUnderAutogen = this->runningUnderAutogen;
    result->censorForSnapshotTests = this->censorForSnapshotTests;
    result->sleepInSlowPath = this->sleepInSlowPath;

    if (keepId) {
        result->globalStateId = this->globalStateId;
    }
    result->deepCloneHistory = this->deepCloneHistory;
    result->deepCloneHistory.emplace_back(DeepCloneHistoryEntry{this->globalStateId, namesUsed()});

    result->strings = this->strings;
    result->stringsLastPageUsed = STRINGS_PAGE_SIZE;
    result->files = this->files;
    result->fileRefByPath = this->fileRefByPath;
    result->lspQuery = this->lspQuery;
    result->kvstoreUuid = this->kvstoreUuid;
    result->lspTypecheckCount = this->lspTypecheckCount;
    result->errorUrlBase = this->errorUrlBase;
    result->ignoredForSuggestTypedErrorClasses = this->ignoredForSuggestTypedErrorClasses;
    result->suppressedErrorClasses = this->suppressedErrorClasses;
    result->onlyErrorClasses = this->onlyErrorClasses;
    result->dslPlugins = this->dslPlugins;
    result->dslRubyExtraArgs = this->dslRubyExtraArgs;
    result->names.reserve(this->names.capacity());
    if (keepId) {
        result->names.resize(this->names.size());
        ::memcpy(result->names.data(), this->names.data(), this->names.size() * sizeof(Name));
    } else {
        for (auto &nm : this->names) {
            result->names.emplace_back(nm.deepCopy(*result));
        }
    }

    result->namesByHash.reserve(this->namesByHash.size());
    result->namesByHash = this->namesByHash;

    result->classAndModules.reserve(this->classAndModules.capacity());
    for (auto &sym : this->classAndModules) {
        result->classAndModules.emplace_back(sym.deepCopy(*result, keepId));
    }
    result->methods.reserve(this->methods.capacity());
    for (auto &sym : this->methods) {
        result->methods.emplace_back(sym.deepCopy(*result, keepId));
    }
    result->fields.reserve(this->fields.capacity());
    for (auto &sym : this->fields) {
        result->fields.emplace_back(sym.deepCopy(*result, keepId));
    }
    result->typeArguments.reserve(this->typeArguments.capacity());
    for (auto &sym : this->typeArguments) {
        result->typeArguments.emplace_back(sym.deepCopy(*result, keepId));
    }
    result->typeMembers.reserve(this->typeMembers.capacity());
    for (auto &sym : this->typeMembers) {
        result->typeMembers.emplace_back(sym.deepCopy(*result, keepId));
    }
    result->pathPrefix = this->pathPrefix;
    for (auto &semanticExtension : this->semanticExtensions) {
        result->semanticExtensions.emplace_back(semanticExtension->deepCopy(*this, *result));
    }
    result->sanityCheck();
    {
        Timer timeit2(tracer(), "GlobalState::deepCopyOut");
        result->creation = timeit2.getFlowEdge();
    }
    return result;
}

string_view GlobalState::getPrintablePath(string_view path) const {
    // Only strip the path prefix if the path has it.
    if (path.substr(0, pathPrefix.length()) == pathPrefix) {
        return path.substr(pathPrefix.length());
    }
    return path;
}

int GlobalState::totalErrors() const {
    return errorQueue->nonSilencedErrorCount.load();
}

void GlobalState::_error(unique_ptr<Error> error) const {
    if (error->isCritical()) {
        errorQueue->hadCritical = true;
    }
    auto loc = error->loc;
    if (loc.file().exists() && ignoredForSuggestTypedErrorClasses.count(error->what.code) == 0) {
        loc.file().data(*this).minErrorLevel_ = min(loc.file().data(*this).minErrorLevel_, error->what.minLevel);
    }

    errorQueue->pushError(*this, move(error));
}

bool GlobalState::hadCriticalError() const {
    return errorQueue->hadCritical;
}

ErrorBuilder GlobalState::beginError(Loc loc, ErrorClass what) const {
    if (what == errors::Internal::InternalError) {
        Exception::failInFuzzer();
    }
    return ErrorBuilder(*this, shouldReportErrorOn(loc, what), loc, what);
}

void GlobalState::ignoreErrorClassForSuggestTyped(int code) {
    ignoredForSuggestTypedErrorClasses.insert(code);
}

void GlobalState::suppressErrorClass(int code) {
    ENFORCE(onlyErrorClasses.empty());
    suppressedErrorClasses.insert(code);
}

void GlobalState::onlyShowErrorClass(int code) {
    ENFORCE(suppressedErrorClasses.empty());
    onlyErrorClasses.insert(code);
}

void GlobalState::addDslPlugin(string_view method, string_view command) {
    auto ref = enterNameUTF8(method);
    auto [it, inserted] = dslPlugins.try_emplace(ref, command);
    if (!inserted) {
        if (auto e = beginError(Loc::none(), errors::Internal::InternalError)) {
            e.setHeader("Duplicate plugin trigger \"{}\". Previous definition: \"{}\"", method, it->second);
        }
    }
}

optional<string_view> GlobalState::findDslPlugin(NameRef method) const {
    const auto it = dslPlugins.find(method);
    if (it != dslPlugins.end()) {
        return it->second;
    }
    return nullopt;
}

bool GlobalState::hasAnyDslPlugin() const {
    return !dslPlugins.empty();
}

bool GlobalState::shouldReportErrorOn(Loc loc, ErrorClass what) const {
    if (what.minLevel == StrictLevel::Internal) {
        return true;
    }
    if (this->silenceErrors) {
        return false;
    }
    if (suppressedErrorClasses.count(what.code) != 0) {
        return false;
    }
    if (!onlyErrorClasses.empty() && onlyErrorClasses.count(what.code) == 0) {
        return false;
    }
    if (!lspQuery.isEmpty()) {
        // LSP queries throw away the errors anyways (only cares about the QueryResponses)
        // so it's no use spending time computing better error messages.
        return false;
    }

    StrictLevel level = StrictLevel::Strong;
    if (loc.file().exists()) {
        level = loc.file().data(*this).strictLevel;
    }
    if (level >= StrictLevel::Max) {
        // Custom rules
        if (level == StrictLevel::Autogenerated) {
            level = StrictLevel::True;
            if (what == errors::Resolver::StubConstant || what == errors::Infer::UntypedMethod) {
                return false;
            }
        } else if (level == StrictLevel::Stdlib) {
            level = StrictLevel::Strict;
            if (what == errors::Resolver::OverloadNotAllowed || what == errors::Resolver::VariantTypeMemberInClass ||
                what == errors::Infer::UntypedMethod) {
                return false;
            }
        }
    }
    ENFORCE(level <= StrictLevel::Strong);

    return level >= what.minLevel;
}

bool GlobalState::wasModified() const {
    return wasModified_;
}

void GlobalState::trace(string_view msg) const {
    errorQueue->tracer.trace(msg);
}

void GlobalState::markAsPayload() {
    bool seenEmpty = false;
    for (auto &f : files) {
        if (!seenEmpty) {
            ENFORCE(!f);
            seenEmpty = true;
            continue;
        }
        f->sourceType = File::Type::Payload;
    }
}

unique_ptr<GlobalState> GlobalState::replaceFile(unique_ptr<GlobalState> inWhat, FileRef whatFile,
                                                 const shared_ptr<File> &withWhat) {
    ENFORCE(whatFile.id() < inWhat->filesUsed());
    ENFORCE(whatFile.dataAllowingUnsafe(*inWhat).path() == withWhat->path());
    inWhat->files[whatFile.id()] = withWhat;
    return inWhat;
}

FileRef GlobalState::findFileByPath(string_view path) const {
    auto fnd = fileRefByPath.find(string(path));
    if (fnd != fileRefByPath.end()) {
        return fnd->second;
    }
    return FileRef();
}

unique_ptr<GlobalState> GlobalState::markFileAsTombStone(unique_ptr<GlobalState> what, FileRef fref) {
    ENFORCE(fref.id() < what->filesUsed());
    what->files[fref.id()]->sourceType = File::Type::TombStone;
    return what;
}

u4 patchHash(u4 hash) {
    if (hash == GlobalStateHash::HASH_STATE_NOT_COMPUTED) {
        hash = GlobalStateHash::HASH_STATE_NOT_COMPUTED_COLLISION_AVOID;
    } else if (hash == GlobalStateHash::HASH_STATE_INVALID) {
        hash = GlobalStateHash::HASH_STATE_INVALID_COLLISION_AVOID;
    }
    return hash;
}

unique_ptr<GlobalStateHash> GlobalState::hash() const {
    constexpr bool DEBUG_HASHING_TAIL = false;
    u4 hierarchyHash = 0;
    UnorderedMap<NameHash, u4> methodHashes;
    int counter = 0;

    for (const auto *symbolType : {&this->classAndModules, &this->fields, &this->typeArguments, &this->typeMembers}) {
        counter = 0;
        for (const auto &sym : *symbolType) {
            if (!sym.ignoreInHashing(*this)) {
                hierarchyHash = mix(hierarchyHash, sym.hash(*this));
                counter++;
                if (DEBUG_HASHING_TAIL && counter > symbolType->size() - 15) {
                    errorQueue->logger.info("Hashing symbols: {}, {}", hierarchyHash, sym.name.show(*this));
                }
            }
        }
    }

    counter = 0;
    for (const auto &sym : this->methods) {
        if (!sym.ignoreInHashing(*this)) {
            auto &target = methodHashes[NameHash(*this, sym.name.data(*this))];
            target = mix(target, sym.hash(*this));
            hierarchyHash = mix(hierarchyHash, sym.methodShapeHash(*this));
            counter++;
            if (DEBUG_HASHING_TAIL && counter > this->methods.size() - 15) {
                errorQueue->logger.info("Hashing method symbols: {}, {}", hierarchyHash, sym.name.show(*this));
            }
        }
    }

    unique_ptr<GlobalStateHash> result = make_unique<GlobalStateHash>();
    for (const auto &e : methodHashes) {
        result->methodHashes.emplace_back(e.first, patchHash(e.second));
    }
    // Sort the hashes. Semantically important for quickly diffing hashes.
    fast_sort(result->methodHashes);

    result->hierarchyHash = patchHash(hierarchyHash);
    return result;
}

const vector<shared_ptr<File>> &GlobalState::getFiles() const {
    return files;
}

SymbolRef GlobalState::staticInitForClass(SymbolRef klass, Loc loc) {
    auto prevCount = methodsUsed();
    auto sym = enterMethodSymbol(loc, klass.data(*this)->singletonClass(*this), core::Names::staticInit());
    if (prevCount != methodsUsed()) {
        auto blkLoc = core::Loc::none(loc.file());
        auto &blkSym = enterMethodArgumentSymbol(blkLoc, sym, core::Names::blkArg());
        blkSym.flags.isBlock = true;
    }
    return sym;
}

SymbolRef GlobalState::lookupStaticInitForClass(SymbolRef klass) const {
    auto &classData = klass.data(*this);
    ENFORCE(classData->isClassOrModule());
    auto ref = classData->lookupSingletonClass(*this).data(*this)->findMember(*this, core::Names::staticInit());
    ENFORCE(ref.exists(), "looking up non-existent <static-init> for {}", klass.toString(*this));
    return ref;
}

SymbolRef GlobalState::staticInitForFile(Loc loc) {
    auto nm = freshNameUnique(core::UniqueNameKind::Namer, core::Names::staticInit(), loc.file().id());
    auto prevCount = this->methodsUsed();
    auto sym = enterMethodSymbol(loc, core::Symbols::rootSingleton(), nm);
    if (prevCount != this->methodsUsed()) {
        auto blkLoc = core::Loc::none(loc.file());
        auto &blkSym = this->enterMethodArgumentSymbol(blkLoc, sym, core::Names::blkArg());
        blkSym.flags.isBlock = true;
    }
    return sym;
}

SymbolRef GlobalState::lookupStaticInitForFile(Loc loc) const {
    auto nm = lookupNameUnique(core::UniqueNameKind::Namer, core::Names::staticInit(), loc.file().id());
    auto ref = core::Symbols::rootSingleton().data(*this)->findMember(*this, nm);
    ENFORCE(ref.exists(), "looking up non-existent <static-init> for {}", loc.toString(*this));
    return ref;
}

spdlog::logger &GlobalState::tracer() const {
    return errorQueue->tracer;
}
} // namespace sorbet::core
