// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/TypeVar.h"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Common.h"
#include "Luau/DenseHash.h"
#include "Luau/Error.h"
#include "Luau/RecursionCounter.h"
#include "Luau/StringUtils.h"
#include "Luau/ToString.h"
#include "Luau/TypeInfer.h"
#include "Luau/TypePack.h"
#include "Luau/VisitTypeVar.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

LUAU_FASTFLAG(DebugLuauFreezeArena)

LUAU_FASTINTVARIABLE(LuauTypeMaximumStringifierLength, 500)
LUAU_FASTINTVARIABLE(LuauTableTypeMaximumStringifierLength, 0)
LUAU_FASTINT(LuauTypeInferRecursionLimit)
LUAU_FASTFLAG(LuauUnknownAndNeverType)
LUAU_FASTFLAGVARIABLE(LuauMaybeGenericIntersectionTypes, false)
LUAU_FASTFLAGVARIABLE(LuauNoMoreGlobalSingletonTypes, false)
LUAU_FASTFLAG(LuauInstantiateInSubtyping)

namespace Luau
{

std::optional<WithPredicate<TypePackId>> magicFunctionFormat(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate);

static std::optional<WithPredicate<TypePackId>> magicFunctionGmatch(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate);

static std::optional<WithPredicate<TypePackId>> magicFunctionMatch(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate);

static std::optional<WithPredicate<TypePackId>> magicFunctionFind(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate);

TypeId follow(TypeId t)
{
    return follow(t, [](TypeId t) {
        return t;
    });
}

TypeId follow(TypeId t, std::function<TypeId(TypeId)> mapper)
{
    auto advance = [&mapper](TypeId ty) -> std::optional<TypeId> {
        if (auto btv = get<Unifiable::Bound<TypeId>>(mapper(ty)))
            return btv->boundTo;
        else if (auto ttv = get<TableTypeVar>(mapper(ty)))
            return ttv->boundTo;
        else if (auto utv = get<UseTypeVar>(mapper(ty)))
        {
            std::optional<TypeId> ty = utv->scope->lookup(utv->def);
            if (!ty)
                throw std::runtime_error("UseTypeVar must map to another TypeId");
            return *ty;
        }
        else
            return std::nullopt;
    };

    auto force = [&mapper](TypeId ty) {
        if (auto ltv = get_if<LazyTypeVar>(&mapper(ty)->ty))
        {
            TypeId res = ltv->thunk();
            if (get<LazyTypeVar>(res))
                throw std::runtime_error("Lazy TypeVar cannot resolve to another Lazy TypeVar");

            *asMutable(ty) = BoundTypeVar(res);
        }
    };

    force(t);

    TypeId cycleTester = t; // Null once we've determined that there is no cycle
    if (auto a = advance(cycleTester))
        cycleTester = *a;
    else
        return t;

    while (true)
    {
        force(t);
        auto a1 = advance(t);
        if (a1)
            t = *a1;
        else
            return t;

        if (nullptr != cycleTester)
        {
            auto a2 = advance(cycleTester);
            if (a2)
            {
                auto a3 = advance(*a2);
                if (a3)
                    cycleTester = *a3;
                else
                    cycleTester = nullptr;
            }
            else
                cycleTester = nullptr;

            if (t == cycleTester)
                throw std::runtime_error("Luau::follow detected a TypeVar cycle!!");
        }
    }
}

std::vector<TypeId> flattenIntersection(TypeId ty)
{
    if (!get<IntersectionTypeVar>(follow(ty)))
        return {ty};

    std::unordered_set<TypeId> seen;
    std::deque<TypeId> queue{ty};

    std::vector<TypeId> result;

    while (!queue.empty())
    {
        TypeId current = follow(queue.front());
        queue.pop_front();

        if (seen.find(current) != seen.end())
            continue;

        seen.insert(current);

        if (auto itv = get<IntersectionTypeVar>(current))
        {
            for (TypeId ty : itv->parts)
                queue.push_back(ty);
        }
        else
            result.push_back(current);
    }

    return result;
}

bool isPrim(TypeId ty, PrimitiveTypeVar::Type primType)
{
    auto p = get<PrimitiveTypeVar>(follow(ty));
    return p && p->type == primType;
}

bool isNil(TypeId ty)
{
    return isPrim(ty, PrimitiveTypeVar::NilType);
}

bool isBoolean(TypeId ty)
{
    if (isPrim(ty, PrimitiveTypeVar::Boolean) || get<BooleanSingleton>(get<SingletonTypeVar>(follow(ty))))
        return true;

    if (auto utv = get<UnionTypeVar>(follow(ty)))
        return std::all_of(begin(utv), end(utv), isBoolean);

    return false;
}

bool isNumber(TypeId ty)
{
    return isPrim(ty, PrimitiveTypeVar::Number);
}

// Returns true when ty is a subtype of string
bool isString(TypeId ty)
{
    ty = follow(ty);

    if (isPrim(ty, PrimitiveTypeVar::String) || get<StringSingleton>(get<SingletonTypeVar>(ty)))
        return true;

    if (auto utv = get<UnionTypeVar>(ty))
        return std::all_of(begin(utv), end(utv), isString);

    return false;
}

// Returns true when ty is a supertype of string
bool maybeString(TypeId ty)
{
    ty = follow(ty);

    if (isPrim(ty, PrimitiveTypeVar::String) || get<AnyTypeVar>(ty))
        return true;

    if (auto utv = get<UnionTypeVar>(ty))
        return std::any_of(begin(utv), end(utv), maybeString);

    return false;
}

bool isThread(TypeId ty)
{
    return isPrim(ty, PrimitiveTypeVar::Thread);
}

bool isOptional(TypeId ty)
{
    if (isNil(ty))
        return true;

    ty = follow(ty);

    if (get<AnyTypeVar>(ty) || (FFlag::LuauUnknownAndNeverType && get<UnknownTypeVar>(ty)))
        return true;

    auto utv = get<UnionTypeVar>(ty);
    if (!utv)
        return false;

    return std::any_of(begin(utv), end(utv), isOptional);
}

bool isTableIntersection(TypeId ty)
{
    if (!get<IntersectionTypeVar>(follow(ty)))
        return false;

    std::vector<TypeId> parts = flattenIntersection(ty);
    return std::all_of(parts.begin(), parts.end(), getTableType);
}

bool isOverloadedFunction(TypeId ty)
{
    if (!get<IntersectionTypeVar>(follow(ty)))
        return false;

    auto isFunction = [](TypeId part) -> bool {
        return get<FunctionTypeVar>(part);
    };

    std::vector<TypeId> parts = flattenIntersection(ty);
    return std::all_of(parts.begin(), parts.end(), isFunction);
}

std::optional<TypeId> getMetatable(TypeId type, NotNull<SingletonTypes> singletonTypes)
{
    type = follow(type);

    if (const MetatableTypeVar* mtType = get<MetatableTypeVar>(type))
        return mtType->metatable;
    else if (const ClassTypeVar* classType = get<ClassTypeVar>(type))
        return classType->metatable;
    else if (isString(type))
    {
        auto ptv = get<PrimitiveTypeVar>(singletonTypes->stringType);
        LUAU_ASSERT(ptv && ptv->metatable);
        return ptv->metatable;
    }

    return std::nullopt;
}

const TableTypeVar* getTableType(TypeId type)
{
    type = follow(type);

    if (const TableTypeVar* ttv = get<TableTypeVar>(type))
        return ttv;
    else if (const MetatableTypeVar* mtv = get<MetatableTypeVar>(type))
        return get<TableTypeVar>(follow(mtv->table));
    else
        return nullptr;
}

TableTypeVar* getMutableTableType(TypeId type)
{
    return const_cast<TableTypeVar*>(getTableType(type));
}

const std::string* getName(TypeId type)
{
    type = follow(type);
    if (auto mtv = get<MetatableTypeVar>(type))
    {
        if (mtv->syntheticName)
            return &*mtv->syntheticName;
        type = follow(mtv->table);
    }

    if (auto ttv = get<TableTypeVar>(type))
    {
        if (ttv->name)
            return &*ttv->name;
        if (ttv->syntheticName)
            return &*ttv->syntheticName;
    }

    return nullptr;
}

std::optional<ModuleName> getDefinitionModuleName(TypeId type)
{
    type = follow(type);

    if (auto ttv = get<TableTypeVar>(type))
    {
        if (!ttv->definitionModuleName.empty())
            return ttv->definitionModuleName;
    }
    else if (auto ftv = get<FunctionTypeVar>(type))
    {
        if (ftv->definition)
            return ftv->definition->definitionModuleName;
    }
    else if (auto ctv = get<ClassTypeVar>(type))
    {
        if (!ctv->definitionModuleName.empty())
            return ctv->definitionModuleName;
    }

    return std::nullopt;
}

bool isSubset(const UnionTypeVar& super, const UnionTypeVar& sub)
{
    std::unordered_set<TypeId> superTypes;

    for (TypeId id : super.options)
        superTypes.insert(id);

    for (TypeId id : sub.options)
    {
        if (superTypes.find(id) == superTypes.end())
            return false;
    }

    return true;
}

// When typechecking an assignment `x = e`, we typecheck `x:T` and `e:U`,
// then instantiate U if `isGeneric(U)` is true, and `maybeGeneric(T)` is false.
bool isGeneric(TypeId ty)
{
    LUAU_ASSERT(!FFlag::LuauInstantiateInSubtyping);

    ty = follow(ty);
    if (auto ftv = get<FunctionTypeVar>(ty))
        return ftv->generics.size() > 0 || ftv->genericPacks.size() > 0;
    else
        // TODO: recurse on type synonyms CLI-39914
        // TODO: recurse on table types CLI-39914
        return false;
}

bool maybeGeneric(TypeId ty)
{
    LUAU_ASSERT(!FFlag::LuauInstantiateInSubtyping);

    if (FFlag::LuauMaybeGenericIntersectionTypes)
    {
        ty = follow(ty);

        if (get<FreeTypeVar>(ty))
            return true;

        if (auto ttv = get<TableTypeVar>(ty))
        {
            // TODO: recurse on table types CLI-39914
            (void)ttv;
            return true;
        }

        if (auto itv = get<IntersectionTypeVar>(ty))
        {
            return std::any_of(begin(itv), end(itv), maybeGeneric);
        }

        return isGeneric(ty);
    }

    ty = follow(ty);
    if (get<FreeTypeVar>(ty))
        return true;
    else if (auto ttv = get<TableTypeVar>(ty))
    {
        // TODO: recurse on table types CLI-39914
        (void)ttv;
        return true;
    }
    else
        return isGeneric(ty);
}

bool maybeSingleton(TypeId ty)
{
    ty = follow(ty);
    if (get<SingletonTypeVar>(ty))
        return true;
    if (const UnionTypeVar* utv = get<UnionTypeVar>(ty))
        for (TypeId option : utv)
            if (get<SingletonTypeVar>(follow(option)))
                return true;
    return false;
}

bool hasLength(TypeId ty, DenseHashSet<TypeId>& seen, int* recursionCount)
{
    RecursionLimiter _rl(recursionCount, FInt::LuauTypeInferRecursionLimit);

    ty = follow(ty);

    if (seen.contains(ty))
        return true;

    if (isString(ty) || get<AnyTypeVar>(ty) || get<TableTypeVar>(ty) || get<MetatableTypeVar>(ty))
        return true;

    if (auto uty = get<UnionTypeVar>(ty))
    {
        seen.insert(ty);

        for (TypeId part : uty->options)
        {
            if (!hasLength(part, seen, recursionCount))
                return false;
        }

        return true;
    }

    if (auto ity = get<IntersectionTypeVar>(ty))
    {
        seen.insert(ty);

        for (TypeId part : ity->parts)
        {
            if (hasLength(part, seen, recursionCount))
                return true;
        }

        return false;
    }

    return false;
}

BlockedTypeVar::BlockedTypeVar()
    : index(++nextIndex)
{
}

int BlockedTypeVar::nextIndex = 0;

PendingExpansionTypeVar::PendingExpansionTypeVar(
    std::optional<AstName> prefix, AstName name, std::vector<TypeId> typeArguments, std::vector<TypePackId> packArguments)
    : prefix(prefix)
    , name(name)
    , typeArguments(typeArguments)
    , packArguments(packArguments)
    , index(++nextIndex)
{
}

size_t PendingExpansionTypeVar::nextIndex = 0;

FunctionTypeVar::FunctionTypeVar(TypePackId argTypes, TypePackId retTypes, std::optional<FunctionDefinition> defn, bool hasSelf)
    : argTypes(argTypes)
    , retTypes(retTypes)
    , definition(std::move(defn))
    , hasSelf(hasSelf)
{
}

FunctionTypeVar::FunctionTypeVar(TypeLevel level, TypePackId argTypes, TypePackId retTypes, std::optional<FunctionDefinition> defn, bool hasSelf)
    : level(level)
    , argTypes(argTypes)
    , retTypes(retTypes)
    , definition(std::move(defn))
    , hasSelf(hasSelf)
{
}

FunctionTypeVar::FunctionTypeVar(
    TypeLevel level, Scope* scope, TypePackId argTypes, TypePackId retTypes, std::optional<FunctionDefinition> defn, bool hasSelf)
    : level(level)
    , scope(scope)
    , argTypes(argTypes)
    , retTypes(retTypes)
    , definition(std::move(defn))
    , hasSelf(hasSelf)
{
}

FunctionTypeVar::FunctionTypeVar(std::vector<TypeId> generics, std::vector<TypePackId> genericPacks, TypePackId argTypes, TypePackId retTypes,
    std::optional<FunctionDefinition> defn, bool hasSelf)
    : generics(generics)
    , genericPacks(genericPacks)
    , argTypes(argTypes)
    , retTypes(retTypes)
    , definition(std::move(defn))
    , hasSelf(hasSelf)
{
}

FunctionTypeVar::FunctionTypeVar(TypeLevel level, std::vector<TypeId> generics, std::vector<TypePackId> genericPacks, TypePackId argTypes,
    TypePackId retTypes, std::optional<FunctionDefinition> defn, bool hasSelf)
    : level(level)
    , generics(generics)
    , genericPacks(genericPacks)
    , argTypes(argTypes)
    , retTypes(retTypes)
    , definition(std::move(defn))
    , hasSelf(hasSelf)
{
}

FunctionTypeVar::FunctionTypeVar(TypeLevel level, Scope* scope, std::vector<TypeId> generics, std::vector<TypePackId> genericPacks,
    TypePackId argTypes, TypePackId retTypes, std::optional<FunctionDefinition> defn, bool hasSelf)
    : level(level)
    , scope(scope)
    , generics(generics)
    , genericPacks(genericPacks)
    , argTypes(argTypes)
    , retTypes(retTypes)
    , definition(std::move(defn))
    , hasSelf(hasSelf)
{
}

TableTypeVar::TableTypeVar(TableState state, TypeLevel level, Scope* scope)
    : state(state)
    , level(level)
    , scope(scope)
{
}

TableTypeVar::TableTypeVar(const Props& props, const std::optional<TableIndexer>& indexer, TypeLevel level, TableState state)
    : props(props)
    , indexer(indexer)
    , state(state)
    , level(level)
{
}

TableTypeVar::TableTypeVar(const Props& props, const std::optional<TableIndexer>& indexer, TypeLevel level, Scope* scope, TableState state)
    : props(props)
    , indexer(indexer)
    , state(state)
    , level(level)
    , scope(scope)
{
}

// Test TypeVars for equivalence
// More complex than we'd like because TypeVars can self-reference.

bool areSeen(SeenSet& seen, const void* lhs, const void* rhs)
{
    if (lhs == rhs)
        return true;

    auto p = std::make_pair(const_cast<void*>(lhs), const_cast<void*>(rhs));
    if (seen.find(p) != seen.end())
        return true;

    seen.insert(p);
    return false;
}

bool areEqual(SeenSet& seen, const FunctionTypeVar& lhs, const FunctionTypeVar& rhs)
{
    if (areSeen(seen, &lhs, &rhs))
        return true;

    // TODO: check generics CLI-39915

    if (!areEqual(seen, *lhs.argTypes, *rhs.argTypes))
        return false;

    if (!areEqual(seen, *lhs.retTypes, *rhs.retTypes))
        return false;

    return true;
}

bool areEqual(SeenSet& seen, const TableTypeVar& lhs, const TableTypeVar& rhs)
{
    if (areSeen(seen, &lhs, &rhs))
        return true;

    if (lhs.state != rhs.state)
        return false;

    if (lhs.props.size() != rhs.props.size())
        return false;

    if (bool(lhs.indexer) != bool(rhs.indexer))
        return false;

    if (lhs.indexer && rhs.indexer)
    {
        if (!areEqual(seen, *lhs.indexer->indexType, *rhs.indexer->indexType))
            return false;

        if (!areEqual(seen, *lhs.indexer->indexResultType, *rhs.indexer->indexResultType))
            return false;
    }

    auto l = lhs.props.begin();
    auto r = rhs.props.begin();

    while (l != lhs.props.end())
    {
        if (l->first != r->first)
            return false;

        if (!areEqual(seen, *l->second.type, *r->second.type))
            return false;
        ++l;
        ++r;
    }

    return true;
}

static bool areEqual(SeenSet& seen, const MetatableTypeVar& lhs, const MetatableTypeVar& rhs)
{
    if (areSeen(seen, &lhs, &rhs))
        return true;

    return areEqual(seen, *lhs.table, *rhs.table) && areEqual(seen, *lhs.metatable, *rhs.metatable);
}

bool areEqual(SeenSet& seen, const TypeVar& lhs, const TypeVar& rhs)
{
    if (auto bound = get_if<BoundTypeVar>(&lhs.ty))
        return areEqual(seen, *bound->boundTo, rhs);

    if (auto bound = get_if<BoundTypeVar>(&rhs.ty))
        return areEqual(seen, lhs, *bound->boundTo);

    if (lhs.ty.index() != rhs.ty.index())
        return false;

    {
        const FreeTypeVar* lf = get_if<FreeTypeVar>(&lhs.ty);
        const FreeTypeVar* rf = get_if<FreeTypeVar>(&rhs.ty);
        if (lf && rf)
            return lf->index == rf->index;
    }

    {
        const GenericTypeVar* lg = get_if<GenericTypeVar>(&lhs.ty);
        const GenericTypeVar* rg = get_if<GenericTypeVar>(&rhs.ty);
        if (lg && rg)
            return lg->index == rg->index;
    }

    {
        const PrimitiveTypeVar* lp = get_if<PrimitiveTypeVar>(&lhs.ty);
        const PrimitiveTypeVar* rp = get_if<PrimitiveTypeVar>(&rhs.ty);
        if (lp && rp)
            return lp->type == rp->type;
    }

    {
        const GenericTypeVar* lg = get_if<GenericTypeVar>(&lhs.ty);
        const GenericTypeVar* rg = get_if<GenericTypeVar>(&rhs.ty);
        if (lg && rg)
            return lg->index == rg->index;
    }

    {
        const ErrorTypeVar* le = get_if<ErrorTypeVar>(&lhs.ty);
        const ErrorTypeVar* re = get_if<ErrorTypeVar>(&rhs.ty);
        if (le && re)
            return le->index == re->index;
    }

    {
        const FunctionTypeVar* lf = get_if<FunctionTypeVar>(&lhs.ty);
        const FunctionTypeVar* rf = get_if<FunctionTypeVar>(&rhs.ty);
        if (lf && rf)
            return areEqual(seen, *lf, *rf);
    }

    {
        const TableTypeVar* lt = get_if<TableTypeVar>(&lhs.ty);
        const TableTypeVar* rt = get_if<TableTypeVar>(&rhs.ty);
        if (lt && rt)
            return areEqual(seen, *lt, *rt);
    }

    {
        const MetatableTypeVar* lmt = get_if<MetatableTypeVar>(&lhs.ty);
        const MetatableTypeVar* rmt = get_if<MetatableTypeVar>(&rhs.ty);

        if (lmt && rmt)
            return areEqual(seen, *lmt, *rmt);
    }

    if (get_if<AnyTypeVar>(&lhs.ty) && get_if<AnyTypeVar>(&rhs.ty))
        return true;

    return false;
}

TypeVar* asMutable(TypeId ty)
{
    return const_cast<TypeVar*>(ty);
}

bool TypeVar::operator==(const TypeVar& rhs) const
{
    SeenSet seen;
    return areEqual(seen, *this, rhs);
}

bool TypeVar::operator!=(const TypeVar& rhs) const
{
    SeenSet seen;
    return !areEqual(seen, *this, rhs);
}

TypeVar& TypeVar::operator=(const TypeVariant& rhs)
{
    ty = rhs;
    return *this;
}

TypeVar& TypeVar::operator=(TypeVariant&& rhs)
{
    ty = std::move(rhs);
    return *this;
}

TypeVar& TypeVar::operator=(const TypeVar& rhs)
{
    LUAU_ASSERT(owningArena == rhs.owningArena);
    LUAU_ASSERT(!rhs.persistent);

    reassign(rhs);

    return *this;
}

TypeId makeFunction(TypeArena& arena, std::optional<TypeId> selfType, std::initializer_list<TypeId> generics,
    std::initializer_list<TypePackId> genericPacks, std::initializer_list<TypeId> paramTypes, std::initializer_list<std::string> paramNames,
    std::initializer_list<TypeId> retTypes);

SingletonTypes::SingletonTypes()
    : arena(new TypeArena)
    , debugFreezeArena(FFlag::DebugLuauFreezeArena)
    , nilType(arena->addType(TypeVar{PrimitiveTypeVar{PrimitiveTypeVar::NilType}, /*persistent*/ true}))
    , numberType(arena->addType(TypeVar{PrimitiveTypeVar{PrimitiveTypeVar::Number}, /*persistent*/ true}))
    , stringType(arena->addType(TypeVar{PrimitiveTypeVar{PrimitiveTypeVar::String}, /*persistent*/ true}))
    , booleanType(arena->addType(TypeVar{PrimitiveTypeVar{PrimitiveTypeVar::Boolean}, /*persistent*/ true}))
    , threadType(arena->addType(TypeVar{PrimitiveTypeVar{PrimitiveTypeVar::Thread}, /*persistent*/ true}))
    , trueType(arena->addType(TypeVar{SingletonTypeVar{BooleanSingleton{true}}, /*persistent*/ true}))
    , falseType(arena->addType(TypeVar{SingletonTypeVar{BooleanSingleton{false}}, /*persistent*/ true}))
    , anyType(arena->addType(TypeVar{AnyTypeVar{}, /*persistent*/ true}))
    , unknownType(arena->addType(TypeVar{UnknownTypeVar{}, /*persistent*/ true}))
    , neverType(arena->addType(TypeVar{NeverTypeVar{}, /*persistent*/ true}))
    , errorType(arena->addType(TypeVar{ErrorTypeVar{}, /*persistent*/ true}))
    , falsyType(arena->addType(TypeVar{UnionTypeVar{{falseType, nilType}}, /*persistent*/ true}))
    , truthyType(arena->addType(TypeVar{NegationTypeVar{falsyType}, /*persistent*/ true}))
    , anyTypePack(arena->addTypePack(TypePackVar{VariadicTypePack{anyType}, /*persistent*/ true}))
    , neverTypePack(arena->addTypePack(TypePackVar{VariadicTypePack{neverType}, /*persistent*/ true}))
    , uninhabitableTypePack(arena->addTypePack({neverType}, neverTypePack))
    , errorTypePack(arena->addTypePack(TypePackVar{Unifiable::Error{}, /*persistent*/ true}))
{
    TypeId stringMetatable = makeStringMetatable();
    asMutable(stringType)->ty = PrimitiveTypeVar{PrimitiveTypeVar::String, stringMetatable};
    persist(stringMetatable);
    persist(uninhabitableTypePack);

    freeze(*arena);
}

SingletonTypes::~SingletonTypes()
{
    // Destroy the arena with the same memory management flags it was created with
    bool prevFlag = FFlag::DebugLuauFreezeArena;
    FFlag::DebugLuauFreezeArena.value = debugFreezeArena;

    unfreeze(*arena);
    arena.reset(nullptr);

    FFlag::DebugLuauFreezeArena.value = prevFlag;
}

TypeId SingletonTypes::makeStringMetatable()
{
    const TypeId optionalNumber = arena->addType(UnionTypeVar{{nilType, numberType}});
    const TypeId optionalString = arena->addType(UnionTypeVar{{nilType, stringType}});
    const TypeId optionalBoolean = arena->addType(UnionTypeVar{{nilType, booleanType}});

    const TypePackId oneStringPack = arena->addTypePack({stringType});
    const TypePackId anyTypePack = arena->addTypePack(TypePackVar{VariadicTypePack{anyType}, true});

    FunctionTypeVar formatFTV{arena->addTypePack(TypePack{{stringType}, anyTypePack}), oneStringPack};
    formatFTV.magicFunction = &magicFunctionFormat;
    const TypeId formatFn = arena->addType(formatFTV);

    const TypePackId emptyPack = arena->addTypePack({});
    const TypePackId stringVariadicList = arena->addTypePack(TypePackVar{VariadicTypePack{stringType}});
    const TypePackId numberVariadicList = arena->addTypePack(TypePackVar{VariadicTypePack{numberType}});

    const TypeId stringToStringType = makeFunction(*arena, std::nullopt, {}, {}, {stringType}, {}, {stringType});

    const TypeId replArgType = arena->addType(
        UnionTypeVar{{stringType, arena->addType(TableTypeVar({}, TableIndexer(stringType, stringType), TypeLevel{}, TableState::Generic)),
            makeFunction(*arena, std::nullopt, {}, {}, {stringType}, {}, {stringType})}});
    const TypeId gsubFunc = makeFunction(*arena, stringType, {}, {}, {stringType, replArgType, optionalNumber}, {}, {stringType, numberType});
    const TypeId gmatchFunc =
        makeFunction(*arena, stringType, {}, {}, {stringType}, {}, {arena->addType(FunctionTypeVar{emptyPack, stringVariadicList})});
    attachMagicFunction(gmatchFunc, magicFunctionGmatch);

    const TypeId matchFunc = arena->addType(
        FunctionTypeVar{arena->addTypePack({stringType, stringType, optionalNumber}), arena->addTypePack(TypePackVar{VariadicTypePack{stringType}})});
    attachMagicFunction(matchFunc, magicFunctionMatch);

    const TypeId findFunc = arena->addType(FunctionTypeVar{arena->addTypePack({stringType, stringType, optionalNumber, optionalBoolean}),
        arena->addTypePack(TypePack{{optionalNumber, optionalNumber}, stringVariadicList})});
    attachMagicFunction(findFunc, magicFunctionFind);

    TableTypeVar::Props stringLib = {
        {"byte", {arena->addType(FunctionTypeVar{arena->addTypePack({stringType, optionalNumber, optionalNumber}), numberVariadicList})}},
        {"char", {arena->addType(FunctionTypeVar{numberVariadicList, arena->addTypePack({stringType})})}},
        {"find", {findFunc}},
        {"format", {formatFn}}, // FIXME
        {"gmatch", {gmatchFunc}},
        {"gsub", {gsubFunc}},
        {"len", {makeFunction(*arena, stringType, {}, {}, {}, {}, {numberType})}},
        {"lower", {stringToStringType}},
        {"match", {matchFunc}},
        {"rep", {makeFunction(*arena, stringType, {}, {}, {numberType}, {}, {stringType})}},
        {"reverse", {stringToStringType}},
        {"sub", {makeFunction(*arena, stringType, {}, {}, {numberType, optionalNumber}, {}, {stringType})}},
        {"upper", {stringToStringType}},
        {"split", {makeFunction(*arena, stringType, {}, {}, {optionalString}, {},
                      {arena->addType(TableTypeVar{{}, TableIndexer{numberType, stringType}, TypeLevel{}, TableState::Sealed})})}},
        {"pack", {arena->addType(FunctionTypeVar{
                     arena->addTypePack(TypePack{{stringType}, anyTypePack}),
                     oneStringPack,
                 })}},
        {"packsize", {makeFunction(*arena, stringType, {}, {}, {}, {}, {numberType})}},
        {"unpack", {arena->addType(FunctionTypeVar{
                       arena->addTypePack(TypePack{{stringType, stringType, optionalNumber}}),
                       anyTypePack,
                   })}},
    };

    assignPropDocumentationSymbols(stringLib, "@luau/global/string");

    TypeId tableType = arena->addType(TableTypeVar{std::move(stringLib), std::nullopt, TypeLevel{}, TableState::Sealed});

    if (TableTypeVar* ttv = getMutable<TableTypeVar>(tableType))
        ttv->name = "string";

    return arena->addType(TableTypeVar{{{{"__index", {tableType}}}}, std::nullopt, TypeLevel{}, TableState::Sealed});
}

TypeId SingletonTypes::errorRecoveryType()
{
    return errorType;
}

TypePackId SingletonTypes::errorRecoveryTypePack()
{
    return errorTypePack;
}

TypeId SingletonTypes::errorRecoveryType(TypeId guess)
{
    return guess;
}

TypePackId SingletonTypes::errorRecoveryTypePack(TypePackId guess)
{
    return guess;
}

SingletonTypes& DEPRECATED_getSingletonTypes()
{
    static SingletonTypes singletonTypes;
    return singletonTypes;
}

void persist(TypeId ty)
{
    std::deque<TypeId> queue{ty};

    while (!queue.empty())
    {
        TypeId t = queue.front();
        queue.pop_front();

        if (t->persistent)
            continue;

        asMutable(t)->persistent = true;

        if (auto btv = get<BoundTypeVar>(t))
            queue.push_back(btv->boundTo);
        else if (auto ftv = get<FunctionTypeVar>(t))
        {
            persist(ftv->argTypes);
            persist(ftv->retTypes);
        }
        else if (auto ttv = get<TableTypeVar>(t))
        {
            LUAU_ASSERT(ttv->state != TableState::Free && ttv->state != TableState::Unsealed);

            for (const auto& [_name, prop] : ttv->props)
                queue.push_back(prop.type);

            if (ttv->indexer)
            {
                queue.push_back(ttv->indexer->indexType);
                queue.push_back(ttv->indexer->indexResultType);
            }
        }
        else if (auto ctv = get<ClassTypeVar>(t))
        {
            for (const auto& [_name, prop] : ctv->props)
                queue.push_back(prop.type);
        }
        else if (auto utv = get<UnionTypeVar>(t))
        {
            for (TypeId opt : utv->options)
                queue.push_back(opt);
        }
        else if (auto itv = get<IntersectionTypeVar>(t))
        {
            for (TypeId opt : itv->parts)
                queue.push_back(opt);
        }
        else if (auto mtv = get<MetatableTypeVar>(t))
        {
            queue.push_back(mtv->table);
            queue.push_back(mtv->metatable);
        }
        else if (get<GenericTypeVar>(t) || get<AnyTypeVar>(t) || get<FreeTypeVar>(t) || get<SingletonTypeVar>(t) || get<PrimitiveTypeVar>(t))
        {
        }
        else
        {
            LUAU_ASSERT(!"TypeId is not supported in a persist call");
        }
    }
}

void persist(TypePackId tp)
{
    if (tp->persistent)
        return;

    asMutable(tp)->persistent = true;

    if (auto p = get<TypePack>(tp))
    {
        for (TypeId ty : p->head)
            persist(ty);
        if (p->tail)
            persist(*p->tail);
    }
    else if (auto vtp = get<VariadicTypePack>(tp))
    {
        persist(vtp->ty);
    }
    else if (get<GenericTypePack>(tp))
    {
    }
    else
    {
        LUAU_ASSERT(!"TypePackId is not supported in a persist call");
    }
}

const TypeLevel* getLevel(TypeId ty)
{
    ty = follow(ty);

    if (auto ftv = get<Unifiable::Free>(ty))
        return &ftv->level;
    else if (auto ttv = get<TableTypeVar>(ty))
        return &ttv->level;
    else if (auto ftv = get<FunctionTypeVar>(ty))
        return &ftv->level;
    else
        return nullptr;
}

TypeLevel* getMutableLevel(TypeId ty)
{
    return const_cast<TypeLevel*>(getLevel(ty));
}

std::optional<TypeLevel> getLevel(TypePackId tp)
{
    tp = follow(tp);

    if (auto ftv = get<Unifiable::Free>(tp))
        return ftv->level;
    else
        return std::nullopt;
}

const Property* lookupClassProp(const ClassTypeVar* cls, const Name& name)
{
    while (cls)
    {
        auto it = cls->props.find(name);
        if (it != cls->props.end())
            return &it->second;

        if (cls->parent)
            cls = get<ClassTypeVar>(*cls->parent);
        else
            return nullptr;

        LUAU_ASSERT(cls);
    }

    return nullptr;
}

bool isSubclass(const ClassTypeVar* cls, const ClassTypeVar* parent)
{
    while (cls)
    {
        if (cls == parent)
            return true;
        else if (!cls->parent)
            return false;

        cls = get<ClassTypeVar>(*cls->parent);
        LUAU_ASSERT(cls);
    }

    return false;
}

const std::vector<TypeId>& getTypes(const UnionTypeVar* utv)
{
    return utv->options;
}

const std::vector<TypeId>& getTypes(const IntersectionTypeVar* itv)
{
    return itv->parts;
}

UnionTypeVarIterator begin(const UnionTypeVar* utv)
{
    return UnionTypeVarIterator{utv};
}

UnionTypeVarIterator end(const UnionTypeVar* utv)
{
    return UnionTypeVarIterator{};
}

IntersectionTypeVarIterator begin(const IntersectionTypeVar* itv)
{
    return IntersectionTypeVarIterator{itv};
}

IntersectionTypeVarIterator end(const IntersectionTypeVar* itv)
{
    return IntersectionTypeVarIterator{};
}

static std::vector<TypeId> parseFormatString(TypeChecker& typechecker, const char* data, size_t size)
{
    const char* options = "cdiouxXeEfgGqs*";

    std::vector<TypeId> result;

    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] == '%')
        {
            i++;

            if (i < size && data[i] == '%')
                continue;

            // we just ignore all characters (including flags/precision) up until first alphabetic character
            while (i < size && !(data[i] > 0 && (isalpha(data[i]) || data[i] == '*')))
                i++;

            if (i == size)
                break;

            if (data[i] == 'q' || data[i] == 's')
                result.push_back(typechecker.stringType);
            else if (data[i] == '*')
                result.push_back(typechecker.unknownType);
            else if (strchr(options, data[i]))
                result.push_back(typechecker.numberType);
            else
                result.push_back(typechecker.errorRecoveryType(typechecker.anyType));
        }
    }

    return result;
}

std::optional<WithPredicate<TypePackId>> magicFunctionFormat(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate)
{
    auto [paramPack, _predicates] = withPredicate;

    TypeArena& arena = typechecker.currentModule->internalTypes;

    AstExprConstantString* fmt = nullptr;
    if (auto index = expr.func->as<AstExprIndexName>(); index && expr.self)
    {
        if (auto group = index->expr->as<AstExprGroup>())
            fmt = group->expr->as<AstExprConstantString>();
        else
            fmt = index->expr->as<AstExprConstantString>();
    }

    if (!expr.self && expr.args.size > 0)
        fmt = expr.args.data[0]->as<AstExprConstantString>();

    if (!fmt)
        return std::nullopt;

    std::vector<TypeId> expected = parseFormatString(typechecker, fmt->value.data, fmt->value.size);
    const auto& [params, tail] = flatten(paramPack);

    size_t paramOffset = 1;
    size_t dataOffset = expr.self ? 0 : 1;

    // unify the prefix one argument at a time
    for (size_t i = 0; i < expected.size() && i + paramOffset < params.size(); ++i)
    {
        Location location = expr.args.data[std::min(i + dataOffset, expr.args.size - 1)]->location;

        typechecker.unify(params[i + paramOffset], expected[i], scope, location);
    }

    // if we know the argument count or if we have too many arguments for sure, we can issue an error
    size_t numActualParams = params.size();
    size_t numExpectedParams = expected.size() + 1; // + 1 for the format string

    if (numExpectedParams != numActualParams && (!tail || numExpectedParams < numActualParams))
        typechecker.reportError(TypeError{expr.location, CountMismatch{numExpectedParams, std::nullopt, numActualParams}});

    return WithPredicate<TypePackId>{arena.addTypePack({typechecker.stringType})};
}

static std::vector<TypeId> parsePatternString(TypeChecker& typechecker, const char* data, size_t size)
{
    std::vector<TypeId> result;
    int depth = 0;
    bool parsingSet = false;

    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] == '%')
        {
            ++i;
            if (!parsingSet && i < size && data[i] == 'b')
                i += 2;
        }
        else if (!parsingSet && data[i] == '[')
        {
            parsingSet = true;
            if (i + 1 < size && data[i + 1] == ']')
                i += 1;
        }
        else if (parsingSet && data[i] == ']')
        {
            parsingSet = false;
        }
        else if (data[i] == '(')
        {
            if (parsingSet)
                continue;

            if (i + 1 < size && data[i + 1] == ')')
            {
                i++;
                result.push_back(typechecker.numberType);
                continue;
            }

            ++depth;
            result.push_back(typechecker.stringType);
        }
        else if (data[i] == ')')
        {
            if (parsingSet)
                continue;

            --depth;

            if (depth < 0)
                break;
        }
    }

    if (depth != 0 || parsingSet)
        return std::vector<TypeId>();

    if (result.empty())
        result.push_back(typechecker.stringType);

    return result;
}

static std::optional<WithPredicate<TypePackId>> magicFunctionGmatch(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate)
{
    auto [paramPack, _predicates] = withPredicate;
    const auto& [params, tail] = flatten(paramPack);

    if (params.size() != 2)
        return std::nullopt;

    TypeArena& arena = typechecker.currentModule->internalTypes;

    AstExprConstantString* pattern = nullptr;
    size_t index = expr.self ? 0 : 1;
    if (expr.args.size > index)
        pattern = expr.args.data[index]->as<AstExprConstantString>();

    if (!pattern)
        return std::nullopt;

    std::vector<TypeId> returnTypes = parsePatternString(typechecker, pattern->value.data, pattern->value.size);

    if (returnTypes.empty())
        return std::nullopt;

    typechecker.unify(params[0], typechecker.stringType, scope, expr.args.data[0]->location);

    const TypePackId emptyPack = arena.addTypePack({});
    const TypePackId returnList = arena.addTypePack(returnTypes);
    const TypeId iteratorType = arena.addType(FunctionTypeVar{emptyPack, returnList});
    return WithPredicate<TypePackId>{arena.addTypePack({iteratorType})};
}

static std::optional<WithPredicate<TypePackId>> magicFunctionMatch(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate)
{
    auto [paramPack, _predicates] = withPredicate;
    const auto& [params, tail] = flatten(paramPack);

    if (params.size() < 2 || params.size() > 3)
        return std::nullopt;

    TypeArena& arena = typechecker.currentModule->internalTypes;

    AstExprConstantString* pattern = nullptr;
    size_t patternIndex = expr.self ? 0 : 1;
    if (expr.args.size > patternIndex)
        pattern = expr.args.data[patternIndex]->as<AstExprConstantString>();

    if (!pattern)
        return std::nullopt;

    std::vector<TypeId> returnTypes = parsePatternString(typechecker, pattern->value.data, pattern->value.size);

    if (returnTypes.empty())
        return std::nullopt;

    typechecker.unify(params[0], typechecker.stringType, scope, expr.args.data[0]->location);

    const TypeId optionalNumber = arena.addType(UnionTypeVar{{typechecker.nilType, typechecker.numberType}});

    size_t initIndex = expr.self ? 1 : 2;
    if (params.size() == 3 && expr.args.size > initIndex)
        typechecker.unify(params[2], optionalNumber, scope, expr.args.data[initIndex]->location);

    const TypePackId returnList = arena.addTypePack(returnTypes);
    return WithPredicate<TypePackId>{returnList};
}

static std::optional<WithPredicate<TypePackId>> magicFunctionFind(
    TypeChecker& typechecker, const ScopePtr& scope, const AstExprCall& expr, WithPredicate<TypePackId> withPredicate)
{
    auto [paramPack, _predicates] = withPredicate;
    const auto& [params, tail] = flatten(paramPack);

    if (params.size() < 2 || params.size() > 4)
        return std::nullopt;

    TypeArena& arena = typechecker.currentModule->internalTypes;

    AstExprConstantString* pattern = nullptr;
    size_t patternIndex = expr.self ? 0 : 1;
    if (expr.args.size > patternIndex)
        pattern = expr.args.data[patternIndex]->as<AstExprConstantString>();

    if (!pattern)
        return std::nullopt;

    bool plain = false;
    size_t plainIndex = expr.self ? 2 : 3;
    if (expr.args.size > plainIndex)
    {
        AstExprConstantBool* p = expr.args.data[plainIndex]->as<AstExprConstantBool>();
        plain = p && p->value;
    }

    std::vector<TypeId> returnTypes;
    if (!plain)
    {
        returnTypes = parsePatternString(typechecker, pattern->value.data, pattern->value.size);

        if (returnTypes.empty())
            return std::nullopt;
    }

    typechecker.unify(params[0], typechecker.stringType, scope, expr.args.data[0]->location);

    const TypeId optionalNumber = arena.addType(UnionTypeVar{{typechecker.nilType, typechecker.numberType}});
    const TypeId optionalBoolean = arena.addType(UnionTypeVar{{typechecker.nilType, typechecker.booleanType}});

    size_t initIndex = expr.self ? 1 : 2;
    if (params.size() >= 3 && expr.args.size > initIndex)
        typechecker.unify(params[2], optionalNumber, scope, expr.args.data[initIndex]->location);

    if (params.size() == 4 && expr.args.size > plainIndex)
        typechecker.unify(params[3], optionalBoolean, scope, expr.args.data[plainIndex]->location);

    returnTypes.insert(returnTypes.begin(), {optionalNumber, optionalNumber});

    const TypePackId returnList = arena.addTypePack(returnTypes);
    return WithPredicate<TypePackId>{returnList};
}

std::vector<TypeId> filterMap(TypeId type, TypeIdPredicate predicate)
{
    type = follow(type);

    if (auto utv = get<UnionTypeVar>(type))
    {
        std::set<TypeId> options;
        for (TypeId option : utv)
            if (auto out = predicate(follow(option)))
                options.insert(*out);

        return std::vector<TypeId>(options.begin(), options.end());
    }
    else if (auto out = predicate(type))
        return {*out};

    return {};
}

static Tags* getTags(TypeId ty)
{
    ty = follow(ty);

    if (auto ftv = getMutable<FunctionTypeVar>(ty))
        return &ftv->tags;
    else if (auto ttv = getMutable<TableTypeVar>(ty))
        return &ttv->tags;
    else if (auto ctv = getMutable<ClassTypeVar>(ty))
        return &ctv->tags;

    return nullptr;
}

void attachTag(TypeId ty, const std::string& tagName)
{
    if (auto tags = getTags(ty))
        tags->push_back(tagName);
    else
        LUAU_ASSERT(!"This TypeId does not support tags");
}

void attachTag(Property& prop, const std::string& tagName)
{
    prop.tags.push_back(tagName);
}

// We would ideally not expose this because it could cause a footgun.
// If the Base class has a tag and you ask if Derived has that tag, it would return false.
// Unfortunately, there's already use cases that's hard to disentangle. For now, we expose it.
bool hasTag(const Tags& tags, const std::string& tagName)
{
    return std::find(tags.begin(), tags.end(), tagName) != tags.end();
}

bool hasTag(TypeId ty, const std::string& tagName)
{
    ty = follow(ty);

    // We special case classes because getTags only returns a pointer to one vector of tags.
    // But classes has multiple vector of tags, represented throughout the hierarchy.
    if (auto ctv = get<ClassTypeVar>(ty))
    {
        while (ctv)
        {
            if (hasTag(ctv->tags, tagName))
                return true;
            else if (!ctv->parent)
                return false;

            ctv = get<ClassTypeVar>(*ctv->parent);
            LUAU_ASSERT(ctv);
        }
    }
    else if (auto tags = getTags(ty))
        return hasTag(*tags, tagName);

    return false;
}

bool hasTag(const Property& prop, const std::string& tagName)
{
    return hasTag(prop.tags, tagName);
}

bool TypeFun::operator==(const TypeFun& rhs) const
{
    return type == rhs.type && typeParams == rhs.typeParams && typePackParams == rhs.typePackParams;
}

bool GenericTypeDefinition::operator==(const GenericTypeDefinition& rhs) const
{
    return ty == rhs.ty && defaultValue == rhs.defaultValue;
}

bool GenericTypePackDefinition::operator==(const GenericTypePackDefinition& rhs) const
{
    return tp == rhs.tp && defaultValue == rhs.defaultValue;
}

} // namespace Luau
