//===--- Expr.cpp - Swift Language Expression ASTs ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the Expr class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Expr.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeLoc.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// Expr methods.
//===----------------------------------------------------------------------===//

// Only allow allocation of Stmts using the allocator in ASTContext.
void *Expr::operator new(size_t Bytes, ASTContext &C,
                         unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

StringRef Expr::getKindName(ExprKind kind) {
  switch (kind) {
#define EXPR(Id, Parent) case ExprKind::Id: return #Id;
#include "swift/AST/ExprNodes.def"
  }
}

// Helper functions to verify statically whether the getSourceRange()
// function has been overridden.
typedef const char (&TwoChars)[2];

template<typename Class> 
inline char checkSourceRangeType(SourceRange (Class::*)() const);

inline TwoChars checkSourceRangeType(SourceRange (Expr::*)() const);

SourceRange Expr::getSourceRange() const {
  switch (Kind) {
#define EXPR(ID, PARENT) \
case ExprKind::ID: \
static_assert(sizeof(checkSourceRangeType(&ID##Expr::getSourceRange)) == 1, \
              #ID "Expr is missing getSourceRange()"); \
return cast<ID##Expr>(this)->getSourceRange();
#include "swift/AST/ExprNodes.def"
  }
  
  llvm_unreachable("expression type not handled!");
}

/// getLoc - Return the caret location of the expression.
SourceLoc Expr::getLoc() const {
  switch (Kind) {
#define EXPR(ID, PARENT) \
  case ExprKind::ID: \
    if (&Expr::getLoc != &ID##Expr::getLoc) \
      return cast<ID##Expr>(this)->getLoc(); \
    break;
#include "swift/AST/ExprNodes.def"
  }

  return getStartLoc();
}

Expr *Expr::getSemanticsProvidingExpr() {
  if (ParenExpr *PE = dyn_cast<ParenExpr>(this))
    return PE->getSubExpr()->getSemanticsProvidingExpr();

  if (DefaultValueExpr *DE = dyn_cast<DefaultValueExpr>(this))
    return DE->getSubExpr()->getSemanticsProvidingExpr();
  
  return this;
}

Expr *Expr::getValueProvidingExpr() {
  // For now, this is totally equivalent to the above.
  // TODO:
  //   - tuple literal projection, which may become interestingly idiomatic
  return getSemanticsProvidingExpr();
}

bool Expr::isImplicit() const {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(this))
    return !DRE->getLoc().isValid();
  
  if (const ImplicitConversionExpr *ICE
        = dyn_cast<ImplicitConversionExpr>(this))
    return ICE->getSubExpr()->isImplicit();
  
  if (const MemberRefExpr *memberRef = dyn_cast<MemberRefExpr>(this))
    return memberRef->getNameLoc().isInvalid();
  if (auto memberRef = dyn_cast<GenericMemberRefExpr>(this))
    return memberRef->getNameLoc().isInvalid();
  if (auto memberRef = dyn_cast<ArchetypeMemberRefExpr>(this))
    return memberRef->getNameLoc().isInvalid();

  if (const MetatypeExpr *metatype = dyn_cast<MetatypeExpr>(this))
    return metatype->getLoc().isInvalid();

  if (const ApplyExpr *apply = dyn_cast<ApplyExpr>(this))
    return apply->getArg() && apply->getArg()->isImplicit();

  if (const TupleExpr *tuple = dyn_cast<TupleExpr>(this)) {
    if (!tuple->getSourceRange().isInvalid())
      return false;

    for (auto elt : tuple->getElements()) {
      if (!elt->isImplicit())
        return false;
    }
    return true;
  }

  if (auto downcast = dyn_cast<UncheckedDowncastExpr>(this)) {
    return downcast->getLoc().isInvalid() &&
           downcast->getSubExpr()->isImplicit();
  }

  if (isa<ZeroValueExpr>(this) || isa<DefaultValueExpr>(this))
    return true;  

  return false;
}

//===----------------------------------------------------------------------===//
// Support methods for Exprs.
//===----------------------------------------------------------------------===//

APInt IntegerLiteralExpr::getValue(StringRef Text,
                                   unsigned BitWidth) {
  llvm::APInt Value(BitWidth, 0);
  // swift encodes octal differently than C
  bool IsCOctal = Text.size() > 1 && Text[0] == '0' && isdigit(Text[1]);
  bool Error = Text.getAsInteger(IsCOctal ? 10 : 0, Value);
  assert(!Error && "Invalid IntegerLiteral formed"); (void)Error;
  if (Value.getBitWidth() != BitWidth)
    Value = Value.zextOrTrunc(BitWidth);
  return Value;
  
}

APInt IntegerLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  return getValue(getText(),
                  getType()->castTo<BuiltinIntegerType>()->getBitWidth());
}

APFloat FloatLiteralExpr::getValue(StringRef Text,
                                   const llvm::fltSemantics &Semantics) {
  APFloat Val(Semantics);
  APFloat::opStatus Res =
    Val.convertFromString(Text, llvm::APFloat::rmNearestTiesToEven);
  assert(Res != APFloat::opInvalidOp && "Sema didn't reject invalid number");
  (void)Res;
  return Val;
}

llvm::APFloat FloatLiteralExpr::getValue() const {
  assert(!getType().isNull() && "Semantic analysis has not completed");
  
  return getValue(getText(),
                  getType()->castTo<BuiltinFloatType>()->getAPFloatSemantics());
}

MemberRefExpr::MemberRefExpr(Expr *Base, SourceLoc DotLoc, VarDecl *Value,
                             SourceLoc NameLoc)
  : Expr(ExprKind::MemberRef), Base(Base),
    Value(Value), DotLoc(DotLoc), NameLoc(NameLoc) { }

ExistentialMemberRefExpr::ExistentialMemberRefExpr(Expr *Base, SourceLoc DotLoc,
                                                   ValueDecl *Value,
                                                   SourceLoc NameLoc)
  : Expr(ExprKind::ExistentialMemberRef), Base(Base), Value(Value),
    DotLoc(DotLoc), NameLoc(NameLoc) { }

ArchetypeMemberRefExpr::ArchetypeMemberRefExpr(Expr *Base, SourceLoc DotLoc,
                                               ValueDecl *Value,
                                               SourceLoc NameLoc)
  : Expr(ExprKind::ArchetypeMemberRef), Base(Base), Value(Value),
    DotLoc(DotLoc), NameLoc(NameLoc) { }

ArchetypeType *ArchetypeMemberRefExpr::getArchetype() const {
  Type BaseTy = getBase()->getType()->getRValueType();
  if (auto Meta = BaseTy->getAs<MetaTypeType>())
    return Meta->getInstanceType()->castTo<ArchetypeType>();

  return BaseTy->castTo<ArchetypeType>();
}

bool ArchetypeMemberRefExpr::isBaseIgnored() const {
  if (isa<TypeDecl>(Value))
    return true;

  return false;
}

GenericMemberRefExpr::GenericMemberRefExpr(Expr *Base, SourceLoc DotLoc,
                                           ValueDecl *Value,
                                           SourceLoc NameLoc)
  : Expr(ExprKind::GenericMemberRef), Base(Base), Value(Value),
    DotLoc(DotLoc), NameLoc(NameLoc) { }

bool GenericMemberRefExpr::isBaseIgnored() const {
  if (getBase()->getType()->getRValueType()->is<MetaTypeType>())
    return true;

  if (isa<TypeDecl>(Value))
    return true;

  if (auto Func = dyn_cast<FuncDecl>(Value))
    return Func->isStatic();

  return false;
}

Type OverloadSetRefExpr::getBaseType() const {
  if (isa<OverloadedDeclRefExpr>(this))
    return Type();
  if (auto *DRE = dyn_cast<OverloadedMemberRefExpr>(this)) {
    return DRE->getBase()->getType()->getRValueType();
  }
  
  llvm_unreachable("Unhandled overloaded set reference expression");
}

bool OverloadSetRefExpr::hasBaseObject() const {
  if (Type BaseTy = getBaseType())
    return !BaseTy->is<MetaTypeType>();

  return false;
}

SequenceExpr *SequenceExpr::create(ASTContext &ctx, ArrayRef<Expr*> elements) {
  void *Buffer = ctx.Allocate(sizeof(SequenceExpr) +
                              elements.size() * sizeof(Expr*),
                              alignof(SequenceExpr));
  return ::new(Buffer) SequenceExpr(elements);
}

NewArrayExpr *NewArrayExpr::create(ASTContext &ctx, SourceLoc newLoc,
                                   TypeLoc elementTy, ArrayRef<Bound> bounds) {
  void *buffer = ctx.Allocate(sizeof(NewArrayExpr) +
                              bounds.size() * sizeof(Bound),
                              alignof(NewArrayExpr));
  NewArrayExpr *E =
    ::new (buffer) NewArrayExpr(newLoc, elementTy, bounds.size());
  memcpy(E->getBoundsBuffer(), bounds.data(), bounds.size() * sizeof(Bound));
  return E;
}

SourceRange TupleExpr::getSourceRange() const {
  if (LParenLoc.isValid() && !HasTrailingClosure) {
    assert(RParenLoc.isValid() && "Mismatched parens?");
    return SourceRange(LParenLoc, RParenLoc);
  }
  if (getElements().empty())
    return SourceRange();
  
  SourceLoc Start = LParenLoc.isValid()? LParenLoc
                                       : getElement(0)->getStartLoc();
  SourceLoc End = getElement(getElements().size()-1)->getEndLoc();
  return SourceRange(Start, End);
}

SubscriptExpr::SubscriptExpr(Expr *Base, Expr *Index, SubscriptDecl *D)
  : Expr(ExprKind::Subscript, D? D->getElementType() : Type()),
    D(D), Base(Base), Index(Index) {
  assert((!D ||
          !D->getDeclContext()->getDeclaredTypeOfContext()->isExistentialType())
         && "use ExistentialSubscriptExpr for existential type subscript");
}

ExistentialSubscriptExpr::
ExistentialSubscriptExpr(Expr *Base, Expr *Index, SubscriptDecl *D)
  : Expr(ExprKind::ExistentialSubscript, D? D->getElementType() : Type()),
    D(D), Base(Base), Index(Index) {
  assert(Base->getType()->getRValueType()->isExistentialType() &&
         "use SubscriptExpr for non-existential type subscript");
}

ArchetypeSubscriptExpr::
ArchetypeSubscriptExpr(Expr *Base, Expr *Index, SubscriptDecl *D)
  : Expr(ExprKind::ArchetypeSubscript, D? D->getElementType() : Type()),
    D(D), Base(Base), Index(Index) {
  assert(Base->getType()->getRValueType()->is<ArchetypeType>() &&
         "use SubscriptExpr for non-archetype type subscript");
}

GenericSubscriptExpr::
GenericSubscriptExpr(Expr *Base, Expr *Index, SubscriptDecl *D)
  : Expr(ExprKind::GenericSubscript, D? D->getElementType() : Type()),
    D(D), Base(Base), Index(Index) {
  assert(Base->getType()->getRValueType()->is<BoundGenericType>() &&
         "use SubscriptExpr for non-generic type subscript");
}

ArrayRef<Pattern *> CapturingExpr::getParamPatterns() const {
  if (auto *func = dyn_cast<FuncExpr>(this))
    return func->getArgParamPatterns();
  if (auto *closure = dyn_cast<PipeClosureExpr>(this))
    return closure->getParams();
  if (auto *closure = dyn_cast<ClosureExpr>(this))
    return closure->getParamPatterns();
  llvm_unreachable("unknown capturing expr");
}

FuncExpr *FuncExpr::create(ASTContext &C, SourceLoc funcLoc,
                           ArrayRef<Pattern*> argParams,
                           ArrayRef<Pattern*> bodyParams,
                           TypeLoc fnRetType,
                           BraceStmt *body, DeclContext *parent) {
  assert(argParams.size() == bodyParams.size());
  unsigned nParams = argParams.size();
  void *buf = C.Allocate(sizeof(FuncExpr) + 2 * nParams * sizeof(Pattern*),
                         alignof(FuncExpr));
  FuncExpr *fn = ::new (buf) FuncExpr(funcLoc, nParams, fnRetType,
                                      body, parent);
  for (unsigned i = 0; i != nParams; ++i)
    fn->getParamsBuffer()[i] = argParams[i];
  for (unsigned i = 0; i != nParams; ++i)
    fn->getParamsBuffer()[i+nParams] = bodyParams[i];
  return fn;
}

SourceRange FuncExpr::getSourceRange() const {
  if (Body)
    return { FuncLoc, Body->getEndLoc() };
  if (FnRetType.hasLocation())
    return { FuncLoc, FnRetType.getSourceRange().End };
  Pattern *LastPat = getArgParamPatterns().back();
  return { FuncLoc, LastPat->getEndLoc() };
}

Type FuncExpr::getResultType(ASTContext &Ctx) const {
  Type resultTy = getType();
  if (!resultTy || resultTy->is<ErrorType>())
    return resultTy;

  for (unsigned i = 0, e = getNumParamPatterns(); i != e; ++i)
    resultTy = resultTy->castTo<AnyFunctionType>()->getResult();

  if (!resultTy)
    resultTy = TupleType::getEmpty(Ctx);

  return resultTy;
}

static ValueDecl *getCalledValue(Expr *E) {
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  Expr *E2 = E->getValueProvidingExpr();
  if (E != E2) return getCalledValue(E2);
  return nullptr;
}

ValueDecl *ApplyExpr::getCalledValue() const {
  return ::getCalledValue(Fn);
}

/// getImplicitThisDecl - If this FuncExpr is a non-static method in an
/// extension context, it will have a 'this' argument.  This method returns it
/// if present, or returns null if not.
VarDecl *FuncExpr::getImplicitThisDecl() const {
  if (getNumParamPatterns() == 0) return nullptr;
  
  // "this" is represented as (typed_pattern (named_pattern (var_decl 'this')).
  TypedPattern *TP = dyn_cast<TypedPattern>(getArgParamPatterns()[0]);
  if (TP == 0) return nullptr;
  
  // The decl should be named 'this' and have no location information.
  NamedPattern *NP = dyn_cast<NamedPattern>(TP->getSubPattern());
  if (NP && NP->getBoundName().str() == "this" && !NP->getLoc().isValid())
    return NP->getDecl();
  return nullptr;
}

RebindThisInConstructorExpr::RebindThisInConstructorExpr(Expr *SubExpr,
                                                         ValueDecl *This)
  : Expr(ExprKind::RebindThisInConstructor,
         TupleType::getEmpty(This->getASTContext())),
    SubExpr(SubExpr), This(This)
{}


SourceRange PipeClosureExpr::getSourceRange() const {
  return body.getPointer()->getSourceRange();
}

SourceLoc PipeClosureExpr::getLoc() const {
  return body.getPointer()->getStartLoc();
}

Expr *PipeClosureExpr::getSingleExpressionBody() const {
  assert(hasSingleExpressionBody() && "Not a single-expression body");
  return cast<ReturnStmt>(body.getPointer()->getElements()[0].get<Stmt *>())
           ->getResult();
}

Type PipeClosureExpr::getResultType() const {
  if (getType()->is<ErrorType>())
    return getType();

  return getType()->castTo<AnyFunctionType>()->getResult();
}

void PipeClosureExpr::setSingleExpressionBody(Expr *newBody) {
  cast<ReturnStmt>(body.getPointer()->getElements()[0].get<Stmt *>())
    ->setResult(newBody);
}

//===----------------------------------------------------------------------===//
// Printing for Expr and all subclasses.
//===----------------------------------------------------------------------===//

namespace {
/// PrintExpr - Visitor implementation of Expr::print.
class PrintExpr : public ExprVisitor<PrintExpr> {
public:
  raw_ostream &OS;
  unsigned Indent;
  
  PrintExpr(raw_ostream &os, unsigned indent) : OS(os), Indent(indent) {
  }
  
  void printRec(Expr *E) {
    Indent += 2;
    if (E)
      visit(E);
    else
      OS.indent(Indent) << "(**NULL EXPRESSION**)";
    Indent -= 2;
  }
  
  /// FIXME: This should use ExprWalker to print children.
  
  void printRec(Decl *D) { D->dump(Indent+2); }
  void printRec(Stmt *S) { S->print(OS, Indent+2); }

  void printSubstitutions(ArrayRef<Substitution> Substitutions) {
    for (auto S : Substitutions) {
      OS.indent(Indent + 2) << "(with " << S.Archetype->getFullName()
                            << " = " << S.Replacement.getString() << ")\n";
    }
  }

  raw_ostream &printCommon(Expr *E, const char *C) {
    return OS.indent(Indent) << '(' << C << " type='" << E->getType() << '\'';
  }

  void visitErrorExpr(ErrorExpr *E) {
    printCommon(E, "error_expr") << ')';
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    printCommon(E, "integer_literal_expr") << " value=";
    if (E->getType().isNull() || E->getType()->isUnresolvedType())
      OS << E->getText();
    else
      OS << E->getValue();
    OS << ')';
  }
  void visitFloatLiteralExpr(FloatLiteralExpr *E) {
    printCommon(E, "float_literal_expr") << " value=" << E->getText() << ')';
  }
  void visitCharacterLiteralExpr(CharacterLiteralExpr *E) {
    printCommon(E, "character_literal_expr") << " value=" << E->getValue()<<')';
  }
  void visitStringLiteralExpr(StringLiteralExpr *E) {
    printCommon(E, "string_literal_expr") << " value=" << E->getValue() << ')';
  }
  void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E) {
    printCommon(E, "interpolated_string_literal_expr");
    for (auto Segment : E->getSegments()) {
      OS << '\n';
      printRec(Segment);
    }
    OS << ')';
  }
  void visitDeclRefExpr(DeclRefExpr *E) {
    printCommon(E, "declref_expr")
      << " decl=" << E->getDecl()->getName() << ')';
  }
  void visitSuperRefExpr(SuperRefExpr *E) {
    printCommon(E, "super_ref_expr") << ')';
  }
  void visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *E) {
    printCommon(E, "other_constructor_ref_expr") << ')';
  }
  void visitUnresolvedConstructorExpr(UnresolvedConstructorExpr *E) {
    printCommon(E, "unresolved_constructor") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *E) {
    printCommon(E, "overloaded_decl_ref_expr")
      << " name=" << E->getDecls()[0]->getName().str()
      << " #decls=" << E->getDecls().size();
    for (ValueDecl *D : E->getDecls()) {
      OS << '\n';
      OS.indent(Indent);
      OS << "  type=" << D->getTypeOfReference().getString();
    }
    OS << ')';
  }
  void visitOverloadedMemberRefExpr(OverloadedMemberRefExpr *E) {
    printCommon(E, "overloaded_member_ref_expr")
      << " name=" << E->getDecls()[0]->getName().str()
      << " #decls=" << E->getDecls().size() << "\n";
    printRec(E->getBase());
    for (ValueDecl *D : E->getDecls()) {
      OS << '\n';
      OS.indent(Indent);
      OS << "  type=" << D->getTypeOfReference().getString();
    }
    OS << ')';
  }
  void visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *E) {
    printCommon(E, "unresolved_decl_ref_expr")
      << " name=" << E->getName() << ')';
  }
  void visitUnresolvedIfExpr(UnresolvedIfExpr *E) {
    printCommon(E, "unresolved_if_expr") << ')';
  }
  void visitUnresolvedElseExpr(UnresolvedElseExpr *E) {
    printCommon(E, "unresolved_else_expr") << ')';
  }
  void visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *E) {
    printCommon(E, "unresolved_specialize_expr") << '\n';
    printRec(E->getSubExpr());
    for (TypeLoc T : E->getUnresolvedParams()) {
      OS << '\n';
      OS.indent(Indent+2);
      T.getType()->print(OS);
    }
    OS << ')';
  }
  
  void visitMemberRefExpr(MemberRefExpr *E) {
    printCommon(E, "member_ref_expr")
      << " decl=" << E->getDecl()->getName() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  void visitExistentialMemberRefExpr(ExistentialMemberRefExpr *E) {
    printCommon(E, "existential_member_ref_expr")
    << " decl=" << E->getDecl()->getName() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  void visitArchetypeMemberRefExpr(ArchetypeMemberRefExpr *E) {
    printCommon(E, "archetype_member_ref_expr")
      << " decl=" << E->getDecl()->getName() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  void visitGenericMemberRefExpr(GenericMemberRefExpr *E) {
    printCommon(E, "generic_member_ref_expr")
      << " decl=" << E->getDecl()->getName() << '\n';
    printSubstitutions(E->getSubstitutions());
    printRec(E->getBase());
    OS << ')';
  }
  void visitUnresolvedMemberExpr(UnresolvedMemberExpr *E) {
    printCommon(E, "unresolved_member_expr")
      << " name='" << E->getName() << "')";
  }
  void visitParenExpr(ParenExpr *E) {
    printCommon(E, "paren_expr");
    if (E->hasTrailingClosure())
      OS << " trailing-closure";
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitTupleExpr(TupleExpr *E) {
    printCommon(E, "tuple_expr");
    if (E->hasTrailingClosure())
      OS << " trailing-closure";

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      if (E->getElement(i))
        printRec(E->getElement(i));
      else
        OS.indent(Indent+2) << "<<tuple element default value>>";
    }
    OS << ')';
  }
  void visitArrayExpr(ArrayExpr *E) {
    printCommon(E, "array_expr");
    OS << '\n';
    printRec(E->getSubExpr());
  }
  void visitDictionaryExpr(DictionaryExpr *E) {
    printCommon(E, "dictionary_expr");
    OS << '\n';
    printRec(E->getSubExpr());
  }
  void visitSubscriptExpr(SubscriptExpr *E) {
    printCommon(E, "subscript_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    OS << ')';
  }
  void visitExistentialSubscriptExpr(ExistentialSubscriptExpr *E) {
    printCommon(E, "existential_subscript_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    OS << ')';
  }
  void visitArchetypeSubscriptExpr(ArchetypeSubscriptExpr *E) {
    printCommon(E, "archetype_subscript_expr");
    OS << '\n';
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    OS << ')';
  }
  void visitGenericSubscriptExpr(GenericSubscriptExpr *E) {
    printCommon(E, "generic_subscript_expr");
    OS << '\n';
    printSubstitutions(E->getSubstitutions());
    printRec(E->getBase());
    OS << '\n';
    printRec(E->getIndex());
    OS << ')';
  }
  void visitUnresolvedDotExpr(UnresolvedDotExpr *E) {
    printCommon(E, "unresolved_dot_expr")
      << " field '" << E->getName().str() << "'";
    if (E->getBase()) {
      OS << '\n';
      printRec(E->getBase());
    }
    OS << ')';
  }
  void visitModuleExpr(ModuleExpr *E) {
    printCommon(E, "module_expr") << ')';
  }
  void visitTupleElementExpr(TupleElementExpr *E) {
    printCommon(E, "tuple_element_expr")
      << " field #" << E->getFieldNumber() << '\n';
    printRec(E->getBase());
    OS << ')';
  }
  void visitTupleShuffleExpr(TupleShuffleExpr *E) {
    printCommon(E, "tuple_shuffle_expr") << " elements=[";
    for (unsigned i = 0, e = E->getElementMapping().size(); i != e; ++i) {
      if (i) OS << ", ";
      OS << E->getElementMapping()[i];
    }
    OS << "]\n";
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitFunctionConversionExpr(FunctionConversionExpr *E) {
    printCommon(E, "function_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitErasureExpr(ErasureExpr *E) {
    printCommon(E, "erasure_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitSpecializeExpr(SpecializeExpr *E) {
    printCommon(E, "specialize_expr") << '\n';
    printSubstitutions(E->getSubstitutions());
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitLoadExpr(LoadExpr *E) {
    printCommon(E, "load_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitMaterializeExpr(MaterializeExpr *E) {
    printCommon(E, "materialize_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitRequalifyExpr(RequalifyExpr *E) {
    printCommon(E, "requalify_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitMetatypeConversionExpr(MetatypeConversionExpr *E) {
    printCommon(E, "metatype_conversion_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitDerivedToBaseExpr(DerivedToBaseExpr *E) {
    printCommon(E, "derived_to_base_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitArchetypeToSuperExpr(ArchetypeToSuperExpr *E) {
    printCommon(E, "archetype_to_super_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitScalarToTupleExpr(ScalarToTupleExpr *E) {
    printCommon(E, "scalar_to_tuple_expr");
    OS << " field=" << E->getScalarField();
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitBridgeToBlockExpr(BridgeToBlockExpr *E) {
    printCommon(E, "bridge_to_block") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }

  void visitAddressOfExpr(AddressOfExpr *E) {
    printCommon(E, "address_of_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitSequenceExpr(SequenceExpr *E) {
    printCommon(E, "sequence_expr");
    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      OS << '\n';
      printRec(E->getElement(i));
    }
    OS << ')';
  }
  
  llvm::raw_ostream &printCapturing(CapturingExpr *E, char const *name) {
    printCommon(E, name);
    if (!E->getCaptures().empty()) {
      OS << " captures=(";
      OS << E->getCaptures()[0]->getName();
      for (auto capture : E->getCaptures().slice(1)) {
        OS << ", " << capture->getName();
      }
      OS << ')';
    }
    return OS;
  }
  
  void visitFuncExpr(FuncExpr *E) {
    printCapturing(E, "func_expr");
    if (E->getBody()) {
      OS << '\n';
      printRec(E->getBody());
    }
    OS << ')';
  }
  void visitPipeClosureExpr(PipeClosureExpr *expr) {
    printCapturing(expr, "closure_expr");
    if (expr->hasSingleExpressionBody()) {
      OS << " single-expression\n";
      printRec(expr->getSingleExpressionBody());
    } else
      printRec(expr->getBody());
    OS << ')';
  }
  void visitImplicitClosureExpr(ImplicitClosureExpr *E) {
    printCapturing(E, "implicit_closure_expr") << '\n';
    printRec(E->getBody());
    OS << ')';
  }

  void visitNewArrayExpr(NewArrayExpr *E) {
    printCommon(E, "new_array_expr")
      << " elementType='" << E->getElementTypeLoc().getType() << "'";
    OS << '\n';
    if (E->hasInjectionFunction())
      printRec(E->getInjectionFunction());
    for (auto &bound : E->getBounds()) {
      OS << '\n';
      if (bound.Value)
        printRec(bound.Value);
      else
        OS.indent(Indent + 2) << "(empty bound)";
    }
    OS << ')';
  }

  void visitMetatypeExpr(MetatypeExpr *E) {
    printCommon(E, "metatype_expr");
    if (Expr *base = E->getBase()) {
      OS << '\n';
      printRec(base);
    } else {
      OS << " baseless";
    }
    OS << ")";
  }

  void visitOpaqueValueExpr(OpaqueValueExpr *E) {
    printCommon(E, "opaque_value_expr") << ')';
  }

  void visitZeroValueExpr(ZeroValueExpr *E) {
    printCommon(E, "zero_value_expr") << ')';
  }

  void printApplyExpr(ApplyExpr *E, const char *NodeName) {
    printCommon(E, NodeName);
    if (E->isSuper())
      OS << " super";
    OS << '\n';
    printRec(E->getFn());
    OS << '\n';
    printRec(E->getArg());
    OS << ')';
  }
  
  void visitCallExpr(CallExpr *E) {
    printApplyExpr(E, "call_expr");
  }
  void visitPrefixUnaryExpr(PrefixUnaryExpr *E) {
    printApplyExpr(E, "prefix_unary_expr");
  }
  void visitPostfixUnaryExpr(PostfixUnaryExpr *E) {
    printApplyExpr(E, "postfix_unary_expr");
  }
  void visitBinaryExpr(BinaryExpr *E) {
    printApplyExpr(E, "binary_expr");
  }
  void visitDotSyntaxCallExpr(DotSyntaxCallExpr *E) {
    printApplyExpr(E, "dot_syntax_call_expr");
  }
  void visitConstructorRefCallExpr(ConstructorRefCallExpr *E) {
    printApplyExpr(E, "constructor_ref_call_expr");
  }
  void visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *E) {
    printCommon(E, "dot_syntax_base_ignored") << '\n';
    printRec(E->getLHS());
    OS << '\n';
    printRec(E->getRHS());
    OS << ')';
  }
  void visitCoerceExpr(CoerceExpr *E) {
    printCommon(E, "coerce_expr") << ' ';
    E->getTypeLoc().getType()->print(OS);
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitUncheckedDowncastExpr(UncheckedDowncastExpr *E) {
    printCommon(E, "unchecked_downcast_expr") << ' ';
    E->getTypeLoc().getType()->print(OS);
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitUncheckedSuperToArchetypeExpr(UncheckedSuperToArchetypeExpr *E) {
    printCommon(E, "unchecked_super_to_archetype_expr") << ' ';
    E->getTypeLoc().getType()->print(OS);
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitRebindThisInConstructorExpr(RebindThisInConstructorExpr *E) {
    printCommon(E, "rebind_this_in_constructor_expr") << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitIfExpr(IfExpr *E) {
    printCommon(E, "if_expr") << '\n';
    printRec(E->getCondExpr());
    OS << '\n';
    printRec(E->getThenExpr());
    OS << '\n';
    printRec(E->getElseExpr());
    OS << ')';
  }
  void visitIsSubtypeExpr(IsSubtypeExpr *E) {
    printCommon(E, "is_subtype_expr") << ' ';
    E->getTypeLoc().getType()->print(OS);
    OS << '\n';
    printRec(E->getSubExpr());
    OS << ')';
  }
  void visitDefaultValueExpr(DefaultValueExpr *E) {
    printCommon(E, "default_value_expr") << ' ';
    printRec(E->getSubExpr());
    OS << ')';
  }
};

} // end anonymous namespace.


void Expr::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void Expr::print(raw_ostream &OS, unsigned Indent) const {
  PrintExpr(OS, Indent).visit(const_cast<Expr*>(this));
}
