/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/compiler/expression/simple_variable.h"
#include "hphp/compiler/analysis/function_scope.h"
#include "hphp/compiler/analysis/variable_table.h"
#include "hphp/compiler/analysis/class_scope.h"
#include "hphp/compiler/option.h"
#include "hphp/compiler/builtin_symbols.h"
#include "hphp/compiler/expression/scalar_expression.h"
#include "hphp/parser/hphp.tab.hpp"
#include "hphp/parser/parser.h"

using namespace HPHP;

///////////////////////////////////////////////////////////////////////////////
// constructors/destructors

SimpleVariable::SimpleVariable
(EXPRESSION_CONSTRUCTOR_PARAMETERS,
 const std::string &name,
 const std::string &docComment /* = "" */)
  : Expression(EXPRESSION_CONSTRUCTOR_PARAMETER_VALUES(SimpleVariable)),
    m_name(name), m_docComment(docComment),
    m_sym(nullptr), m_originalSym(nullptr),
    m_this(false), m_globals(false),
    m_superGlobal(false), m_alwaysStash(false) {
  setContext(Expression::NoLValueWrapper);
}

ExpressionPtr SimpleVariable::clone() {
  SimpleVariablePtr exp(new SimpleVariable(*this));
  Expression::deepCopy(exp);
  return exp;
}

///////////////////////////////////////////////////////////////////////////////
// parser functions

///////////////////////////////////////////////////////////////////////////////
// static analysis functions

void SimpleVariable::setContext(Context context) {
  m_context |= context;
  if (m_this) {
    bool ref = context & (RefValue | RefAssignmentLHS);
    bool unset = ((context & Expression::UnsetContext) &&
      (context & Expression::LValue));
    if (ref || unset) {
      if (FunctionScopePtr func = getFunctionScope()) {
        func->setContainsBareThis(true, true);
      }
    }
  }
}

int SimpleVariable::getLocalEffects() const {
  if (m_context == Declaration &&
      m_sym && m_sym->isShrinkWrapped()) {
    return LocalEffect;
  }
  return NoEffect;
}

void SimpleVariable::updateSymbol(SimpleVariablePtr src) {
  m_sym = getScope()->getVariables()->addSymbol(m_name);
  if (src && src->m_sym) {
    m_sym->update(src->m_sym);
  }
}

bool SimpleVariable::couldBeAliased() const {
  if (m_globals || m_superGlobal) return true;
  if (m_name == "http_response_header") return true;
  if (m_name == "php_errormsg") return true;
  always_assert(m_sym);
  if (m_sym->isGlobal() || m_sym->isStatic()) return true;
  if (getScope()->inPseudoMain() && !m_sym->isHidden()) return true;
  if (isReferencedValid()) return isReferenced();
  return m_sym->isReferenced();
}

bool SimpleVariable::isHidden() const {
  return m_sym && m_sym->isHidden();
}

void SimpleVariable::coalesce(SimpleVariablePtr other) {
  always_assert(m_sym);
  always_assert(other->m_sym);
  if (!m_originalSym) m_originalSym = m_sym;
  m_sym->clearUsed();
  m_sym->clearNeeded();
  m_sym = other->m_sym;
  m_name = m_sym->getName();
}

/*
  This simple variable is about to go out of scope.
  Is it ok to kill the last assignment?
  What if its a reference assignment (or an unset)?
*/
bool SimpleVariable::canKill(bool isref) const {
  if (m_globals || m_superGlobal) return false;
  always_assert(m_sym);
  if (m_sym->isGlobal() || m_sym->isStatic()) {
    return isref && !getScope()->inPseudoMain();
  }

  return
    (isref && (m_sym->isHidden() || !getScope()->inPseudoMain())) ||
    (isReferencedValid() ? !isReferenced() : !m_sym->isReferenced());
}

void SimpleVariable::analyzeProgram(AnalysisResultPtr ar) {
  m_superGlobal = BuiltinSymbols::IsSuperGlobal(m_name);
  m_superGlobalType = BuiltinSymbols::GetSuperGlobalType(m_name);

  VariableTablePtr variables = getScope()->getVariables();
  if (m_superGlobal) {
    variables->setAttribute(VariableTable::NeedGlobalPointer);
  } else if (m_name == "GLOBALS") {
    m_globals = true;
  } else {
    m_sym = variables->addDeclaredSymbol(m_name, shared_from_this());
  }

  if (m_name == "http_response_header" || m_name == "php_errormsg") {
    setInited();
  }

  if (ar->getPhase() == AnalysisResult::AnalyzeAll) {
    if (FunctionScopePtr func = getFunctionScope()) {
      if (m_name == "this" && func->mayContainThis()) {
        func->setContainsThis();
        m_this = true;
        if (!hasContext(ObjectContext)) {
          bool unset = hasAllContext(UnsetContext | LValue);
          func->setContainsBareThis(
            true,
            hasAnyContext(RefValue | RefAssignmentLHS) ||
            m_sym->isRefClosureVar() || unset);
          if (variables->getAttribute(VariableTable::ContainsDynamicVariable)) {
            ClassScopePtr cls = getClassScope();
            TypePtr t = !cls || cls->isRedeclaring() ?
              Type::Variant : Type::CreateObjectType(cls->getOriginalName());
            variables->add(m_sym, t, true, ar, shared_from_this(),
                           getScope()->getModifiers());
          }
        }
      }
      if (m_sym && !(m_context & AssignmentLHS) &&
          !((m_context & UnsetContext) && (m_context & LValue))) {
        m_sym->setUsed();
      }
    }
  } else if (ar->getPhase() == AnalysisResult::AnalyzeFinal) {
    if (m_sym && !m_this) {
      if (!m_sym->isSystem() &&
          !(getContext() &
            (LValue|RefValue|RefParameter|UnsetContext|ExistContext)) &&
          m_sym->getDeclaration().get() == this &&
          !m_sym->isParameter()) {

        if (!variables->getAttribute(VariableTable::ContainsLDynamicVariable) &&
            !getScope()->is(BlockScope::ClassScope)) {
          if (getScope()->inPseudoMain()) {
            Compiler::Error(Compiler::UseUndeclaredGlobalVariable,
                            shared_from_this());
          } else if (!m_sym->isClosureVar()) {
            Compiler::Error(Compiler::UseUndeclaredVariable,
                            shared_from_this());
          }
        }
      }
      // check function parameter that can occur in lval context
      if (m_sym->isParameter() &&
          m_context & (LValue | RefValue | DeepReference |
                       UnsetContext | InvokeArgument | OprLValue |
                       DeepOprLValue)) {
        m_sym->setLvalParam();
      }
    }
  }
}

bool SimpleVariable::checkUnused() const {
  return !m_superGlobal && !m_globals &&
    getScope()->getVariables()->checkUnused(m_sym);
}

///////////////////////////////////////////////////////////////////////////////

void SimpleVariable::outputCodeModel(CodeGenerator &cg) {
  cg.printObjectHeader("SimpleVariableExpression", 2);
  cg.printPropertyHeader("variableName");
  cg.printValue(m_name);
  cg.printPropertyHeader("sourceLocation");
  cg.printLocation(this);
  cg.printObjectFooter();
}

///////////////////////////////////////////////////////////////////////////////
// code generation functions

void SimpleVariable::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  cg_printf("$%s", m_name.c_str());
}
