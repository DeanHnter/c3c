// Copyright (c) 2020 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by a LGPLv3.0
// a copy of which can be found in the LICENSE file.

#include "sema_internal.h"

static inline bool sema_analyse_asm_stmt(SemaContext *context, Ast *stmt);
static inline bool sema_analyse_assert_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_break_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_compound_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_continue_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_ct_for_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_ct_foreach_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_ct_if_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_ct_switch_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_declare_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_defer_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_expr_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_for_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_foreach_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_if_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_nextcase_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_return_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_switch_stmt(SemaContext *context, Ast *statement);

static inline bool sema_return_optional_check_is_valid_in_scope(SemaContext *context, Expr *ret_expr);
static inline bool sema_defer_by_result(AstId defer_top, AstId defer_bottom);
static inline bool sema_analyse_block_exit_stmt(SemaContext *context, Ast *statement);
static inline bool sema_analyse_defer_stmt_body(SemaContext *context, Ast *statement, Ast *body);
static inline bool sema_analyse_for_cond(SemaContext *context, ExprId *cond_ref, bool *infinite);
static inline bool assert_create_from_contract(SemaContext *context, Ast *directive, AstId **asserts, SourceSpan evaluation_location);
static bool sema_analyse_asm_string_stmt(SemaContext *context, Ast *stmt);
static void sema_unwrappable_from_catch_in_else(SemaContext *c, Expr *cond);
static inline bool sema_analyse_try_unwrap(SemaContext *context, Expr *expr);
static inline bool sema_analyse_try_unwrap_chain(SemaContext *context, Expr *expr, CondType cond_type);
static void sema_remove_unwraps_from_try(SemaContext *c, Expr *cond);
static inline bool sema_analyse_last_cond(SemaContext *context, Expr *expr, CondType cond_type);
static inline bool sema_analyse_cond_list(SemaContext *context, Expr *expr, CondType cond_type);
static inline bool sema_analyse_cond(SemaContext *context, Expr *expr, CondType cond_type);
static inline Decl *sema_analyse_label(SemaContext *context, Ast *stmt);
static bool context_labels_exist_in_scope(SemaContext *context);
static inline bool sema_analyse_then_overwrite(SemaContext *context, Ast *statement, AstId replacement);
static inline bool sema_analyse_catch_unwrap(SemaContext *context, Expr *expr);
static inline bool sema_analyse_compound_statement_no_scope(SemaContext *context, Ast *compound_statement);
static inline bool sema_check_type_case(SemaContext *context, Type *switch_type, Ast *case_stmt, Ast **cases, unsigned index);
static inline bool sema_check_value_case(SemaContext *context, Type *switch_type, Ast *case_stmt, Ast **cases, unsigned index, bool *if_chained, bool *max_ranged);
static bool sema_analyse_switch_body(SemaContext *context, Ast *statement, SourceSpan expr_span, Type *switch_type, Ast **cases, ExprAnySwitch *any_switch, Decl *var_holder);

static inline bool sema_analyse_statement_inner(SemaContext *context, Ast *statement);
static bool sema_analyse_require(SemaContext *context, Ast *directive, AstId **asserts, SourceSpan source);
static bool sema_analyse_ensure(SemaContext *context, Ast *directive);
static bool sema_analyse_optional_returns(SemaContext *context, Ast *directive);

static inline bool sema_analyse_asm_stmt(SemaContext *context, Ast *stmt)
{
	if (stmt->asm_block_stmt.is_string) return sema_analyse_asm_string_stmt(context, stmt);
	AsmInlineBlock *block = stmt->asm_block_stmt.block;
	AstId ast_id = block->asm_stmt;
	scratch_buffer_clear();
	while (ast_id)
	{
		Ast *ast = astptr(ast_id);
		ast_id = ast->next;
		if (!sema_analyse_asm(context, block, ast)) return false;
	}
	return true;
}

/**
 * assert(foo), assert(foo, message), assert(try foo), assert(try foo, message)
 *
 * - Using the try construct we implicitly unpack the variable if present.
 * - assert(false) will implicitly make the rest of the code marked as unreachable.
 * - assert might also be used for 'ensure' and in this case violating it is a compile time error.
 */
static inline bool sema_analyse_assert_stmt(SemaContext *context, Ast *statement)
{
	Expr *expr = exprptr(statement->assert_stmt.expr);

	// Verify that the message is a string if it exists.
	Expr *message_expr = exprptrzero(statement->assert_stmt.message);
	if (message_expr)
	{
		if (!sema_analyse_expr(context, message_expr)) return false;
		if (!expr_is_const_string(message_expr)) RETURN_SEMA_ERROR(message_expr, "Expected a constant string as the error message.");
		FOREACH_BEGIN(Expr *e, statement->assert_stmt.args)
			if (!sema_analyse_expr(context, e)) return false;
			if (IS_OPTIONAL(e)) RETURN_SEMA_ERROR(e, "Optionals cannot be used as assert arguments, use '?" "?', '!' or '!!' to fix this.");
			if (type_is_void(e->type)) RETURN_SEMA_ERROR(e, "This expression is of type 'void', did you make a mistake?");
		FOREACH_END();
	}

	// Check the conditional inside
	if (!sema_analyse_cond_expr(context, expr)) return false;

	// If it's constant, we process it differently.
	if (expr_is_const(expr))
	{
		// It's true, then replace the statement with a nop.
		if (expr->const_expr.b)
		{
			statement->ast_kind = AST_NOP_STMT;
			return true;
		}
		// If it's ensure (and an error) we print an error.
		if (statement->assert_stmt.is_ensure)
		{
			if (message_expr && expr_is_const(message_expr) && vec_size(statement->assert_stmt.args))
			{
				SEMA_ERROR(expr, "%.*s", EXPAND_EXPR_STRING(message_expr));
			}
			else
			{
				SEMA_ERROR(expr, "Contract violated.");
			}
			return false;
		}
		// assert(false) means this can't be reached.
		context->active_scope.jump_end = true;
	}
	return true;
}

/**
 * break and break LABEL;
 */
static inline bool sema_analyse_break_stmt(SemaContext *context, Ast *statement)
{
	// If there is no break target and there is no label,
	// we skip.
	if (!context->break_target && !statement->contbreak_stmt.is_label)
	{
		if (context_labels_exist_in_scope(context))
		{
			SEMA_ERROR(statement, "Unlabelled 'break' is not allowed here.");
		}
		else
		{
			SEMA_ERROR(statement, "There is no valid target for 'break', did you make a mistake?");
		}
		return false;
	}

	// Is jump, and set it as resolved.
	context->active_scope.jump_end = true;
	statement->contbreak_stmt.is_resolved = true;

	AstId defer_begin;
	Ast *parent;

	if (statement->contbreak_stmt.label.name)
	{
		// If we have a label, pick it and set the parent astid to that target.
		ASSIGN_DECL_OR_RET(Decl *target, sema_analyse_label(context, statement), false);
		// We don't need to do any checking since all(!) label constructs support break.
		parent = astptr(target->label.parent);
		defer_begin = target->label.defer;
	}
	else
	{
		// Jump to the default break target.
		parent = context->break_target;
		defer_begin = context->break_defer;
	}

	assert(parent);
	parent->flow.has_break = true;
	statement->contbreak_stmt.ast = astid(parent);

	// Append the defers.
	statement->contbreak_stmt.defers = context_get_defers(context, context->active_scope.defer_last, defer_begin, true);
	return true;
}

/**
 * The regular { }
 */
static inline bool sema_analyse_compound_stmt(SemaContext *context, Ast *statement)
{
	bool success;
	bool ends_with_jump;
	SCOPE_START
		success = sema_analyse_compound_statement_no_scope(context, statement);
		ends_with_jump = context->active_scope.jump_end;
	SCOPE_END;
	// If this ends with a jump, then we know we don't need to certain analysis.
	context->active_scope.jump_end = ends_with_jump;
	return success;
}

/**
 * continue and continue FOO;
 */
static inline bool sema_analyse_continue_stmt(SemaContext *context, Ast *statement)
{
	// If we have a plain continue and no continue label, we just failed.
	if (!context->continue_target && !statement->contbreak_stmt.label.name)
	{
		SEMA_ERROR(statement, "'continue' is not allowed here.");
		return false;
	}

	AstId defer_id;
	Ast *parent;
	if (statement->contbreak_stmt.label.name)
	{
		// If we have a label grab it.
		ASSIGN_DECL_OR_RET(Decl *target, sema_analyse_label(context, statement), false);
		defer_id = target->label.defer;
		parent = astptr(target->label.parent);
		// Continue can only be used with "for" statements, skipping the "do {  };" statement
		if (!ast_supports_continue(parent))
		{
			SEMA_ERROR(statement, "'continue' may only be used with 'for', 'while' and 'do-while' statements.");
			return false;
		}
	}
	else
	{
		// Use default defer and ast.
		defer_id = context->continue_defer;
		parent = context->continue_target;
	}

	// This makes the active scope jump.
	context->active_scope.jump_end = true;

	// Link the parent and add the defers.
	statement->contbreak_stmt.ast = astid(parent);
	statement->contbreak_stmt.defers = context_get_defers(context, context->active_scope.defer_last, defer_id, true);
	return true;
}

static void sema_unwrappable_from_catch_in_else(SemaContext *c, Expr *cond)
{
	assert(cond->expr_kind == EXPR_COND);

	Expr *last = VECLAST(cond->cond_expr);
	while (last->expr_kind == EXPR_CAST)
	{
		last = exprptr(last->cast_expr.expr);
	}
	if (!last || last->expr_kind != EXPR_CATCH_UNWRAP) return;

	Expr **unwrapped = last->catch_unwrap_expr.exprs;

	VECEACH(unwrapped, i)
	{
		Expr *expr = unwrapped[i];
		if (expr->expr_kind != EXPR_IDENTIFIER) continue;
		Decl *decl = expr->identifier_expr.decl;
		if (decl->decl_kind != DECL_VAR) continue;
		assert(decl->type->type_kind == TYPE_OPTIONAL && "The variable should always be optional at this point.");

		// 5. Locals and globals may be unwrapped
		switch (decl->var.kind)
		{
			case VARDECL_LOCAL:
			case VARDECL_GLOBAL:
				sema_unwrap_var(c, decl);
				break;
			default:
				continue;
		}

	}
}


// --- Sema analyse stmts


static inline bool assert_create_from_contract(SemaContext *context, Ast *directive, AstId **asserts, SourceSpan evaluation_location)
{
	directive = copy_ast_single(directive);
	Expr *declexpr = directive->contract_stmt.contract.decl_exprs;
	assert(declexpr->expr_kind == EXPR_EXPRESSION_LIST);

	Expr **exprs = declexpr->expression_list;

	VECEACH(exprs, j)
	{
		Expr *expr = exprs[j];
		if (expr->expr_kind == EXPR_DECL)
		{
			SEMA_ERROR(expr, "Only expressions are allowed.");
			return false;
		}
		if (!sema_analyse_cond_expr(context, expr)) return false;

		const char *comment = directive->contract_stmt.contract.comment;
		if (!comment) comment = directive->contract_stmt.contract.expr_string;
		if (expr_is_const(expr))
		{
			assert(expr->const_expr.const_kind == CONST_BOOL);
			if (expr->const_expr.b)
			{
				// Verified, so we can skip introducing it.
				continue;
			}
			sema_error_at(evaluation_location.a ? evaluation_location : expr->span, "%s", comment);
			return false;
		}
		Ast *assert = new_ast(AST_ASSERT_STMT, expr->span);
		assert->assert_stmt.is_ensure = true;
		assert->assert_stmt.expr = exprid(expr);
		Expr *comment_expr = expr_new(EXPR_CONST, expr->span);
		expr_rewrite_to_string(comment_expr, comment);
		assert->assert_stmt.message = exprid(comment_expr);
		ast_append(asserts, assert);
	}
	return true;
}

static inline bool sema_defer_by_result(AstId defer_top, AstId defer_bottom)
{
	AstId first = 0;
	AstId *next = &first;
	while (defer_bottom != defer_top)
	{
		Ast *defer = astptr(defer_top);
		if (defer->defer_stmt.is_catch || defer->defer_stmt.is_try) return true;
		defer_top = defer->defer_stmt.prev_defer;
	}
	return false;
}

static inline void sema_inline_return_defers(SemaContext *context, Ast *stmt, AstId defer_top, AstId defer_bottom)
{
	stmt->return_stmt.cleanup = context_get_defers(context, defer_top, defer_bottom, true);
	if (stmt->return_stmt.expr && IS_OPTIONAL(stmt->return_stmt.expr) && sema_defer_by_result(context->active_scope.defer_last, context->block_return_defer))
	{
		stmt->return_stmt.cleanup_fail = context_get_defers(context, context->active_scope.defer_last, context->block_return_defer, false);
	}
	else
	{
		stmt->return_stmt.cleanup_fail = stmt->return_stmt.cleanup ? astid(copy_ast_defer(astptr(stmt->return_stmt.cleanup))) : 0;
	}
}

static inline bool sema_return_optional_check_is_valid_in_scope(SemaContext *context, Expr *ret_expr)
{
	if (!IS_OPTIONAL(ret_expr) || !context->call_env.opt_returns) return true;
	if (ret_expr->expr_kind != EXPR_OPTIONAL) return true;
	Expr *inner = ret_expr->inner_expr;
	if (!expr_is_const(inner)) return true;
	assert(ret_expr->inner_expr->const_expr.const_kind == CONST_ERR);
	Decl *fault = ret_expr->inner_expr->const_expr.enum_err_val;
	FOREACH_BEGIN(Decl *opt, context->call_env.opt_returns)
		if (opt->decl_kind == DECL_FAULT)
		{
			if (fault->type->decl == opt) return true;
			continue;
		}
		if (opt == fault) return true;
	FOREACH_END();
	SEMA_ERROR(ret_expr, "This value does not match declared optional returns, it needs to be declared with the other optional returns.");
	return false;
}

static bool sema_analyse_macro_constant_ensures(SemaContext *context, Expr *ret_expr)
{
	assert(context->current_macro);
	// This is a per return check, so we don't do it if the return expression is missing,
	// or if it is optional, or – obviously - if there are no '@ensure'.
	if (!ret_expr || !context->macro_has_ensures || IS_OPTIONAL(ret_expr)) return true;

	// If the return expression can't be flattened to a constant value, then
	// we won't be able to do any constant ensure checks anyway, so skip.
	if (!sema_flattened_expr_is_const(context, ret_expr)) return true;

	AstId doc_directive = context->current_macro->func_decl.docs;
	// We store the old return_expr for retval
	Expr *return_expr_old = context->return_expr;
	// And set our new one.
	context->return_expr = ret_expr;
	bool success = true;
	SCOPE_START_WITH_FLAGS(SCOPE_ENSURE_MACRO);
		while (doc_directive)
		{
			Ast *directive = astptr(doc_directive);
			doc_directive = directive->next;
			if (directive->contract_stmt.kind != CONTRACT_ENSURE) continue;
			Expr *checks = copy_expr_single(directive->contract_stmt.contract.decl_exprs);
			assert(checks->expr_kind == EXPR_EXPRESSION_LIST);
			Expr **exprs = checks->expression_list;
			FOREACH_BEGIN(Expr *expr, exprs)
				if (expr->expr_kind == EXPR_DECL)
				{
					SEMA_ERROR(expr, "Only expressions are allowed.");
					success = false;
					goto END;
				}
				if (!sema_analyse_cond_expr(context, expr))
				{
					success = false;
					goto END;
				}
				// Skipping non-const.
				if (!expr_is_const(expr)) continue;
				// It was ok.
				assert(expr->const_expr.const_kind == CONST_BOOL);
				if (expr->const_expr.b) continue;
				const char *comment = directive->contract_stmt.contract.comment;
				if (!comment) comment = directive->contract_stmt.contract.expr_string;
				SEMA_ERROR(ret_expr, "%s", comment);
				success = false;
				goto END;
			FOREACH_END();
		}
END:
	SCOPE_END;
	context->return_expr = return_expr_old;
	return success;
}
/**
 * Handle exit in a macro or in an expression block.
 * @param context
 * @param statement
 * @return
 */
static inline bool sema_analyse_block_exit_stmt(SemaContext *context, Ast *statement)
{
	bool is_macro = (context->active_scope.flags & SCOPE_MACRO) != 0;
	assert(context->active_scope.flags & (SCOPE_EXPR_BLOCK | SCOPE_MACRO));
	statement->ast_kind = AST_BLOCK_EXIT_STMT;
	context->active_scope.jump_end = true;
	Type *block_type = context->expected_block_type;
	Expr *ret_expr = statement->return_stmt.expr;
	if (ret_expr)
	{
		if (block_type)
		{
			if (!sema_analyse_expr_rhs(context, block_type, ret_expr, true)) return false;
		}
		else
		{
			if (!sema_analyse_expr(context, ret_expr)) return false;
		}
		if (is_macro && !sema_return_optional_check_is_valid_in_scope(context, ret_expr)) return false;

	}
	else
	{
		if (block_type && type_no_optional(block_type) != type_void)
		{
			SEMA_ERROR(statement, "Expected a return value of type %s here.", type_quoted_error_string(block_type));
			return false;
		}
	}
	statement->return_stmt.block_exit_ref = context->block_exit_ref;
	sema_inline_return_defers(context, statement, context->active_scope.defer_last, context->block_return_defer);

	if (is_macro && !sema_analyse_macro_constant_ensures(context, ret_expr)) return false;
	vec_add(context->returns, statement);
	return true;
}

/**
 * Prevent the common mistake of `return &a` where "a" is a local.
 * @return true if the check is ok (no such escape)
 */
INLINE bool sema_check_not_stack_variable_escape(SemaContext *context, Expr *expr)
{
	Expr *outer = expr;
	while (expr->expr_kind == EXPR_CAST) expr = exprptr(expr->cast_expr.expr);
	// We only want && and &
	if (expr->expr_kind == EXPR_SUBSCRIPT_ADDR)
	{
		expr = exprptr(expr->subscript_expr.expr);
		goto CHECK_ACCESS;
	}
	if (expr->expr_kind != EXPR_UNARY) return true;
	if (expr->unary_expr.operator == UNARYOP_TADDR)
	{
		SEMA_ERROR(outer, "A pointer to a temporary value will be invalid once the function returns. Try copying the value to the heap or the temp memory instead.");
		return false;
	}
	if (expr->unary_expr.operator != UNARYOP_ADDR) return true;
	expr = expr->unary_expr.expr;
CHECK_ACCESS:
	while (expr->expr_kind == EXPR_ACCESS) expr = expr->access_expr.parent;
	if (expr->expr_kind != EXPR_IDENTIFIER) return true;
	Decl *decl = expr->identifier_expr.decl;
	if (decl->decl_kind != DECL_VAR) return true;
	switch (decl->var.kind)
	{
		case VARDECL_LOCAL:
			if (decl->var.is_static) return true;
			FALLTHROUGH;
		case VARDECL_PARAM:
			break;
		default:
			return true;
	}
	SEMA_ERROR(outer, "A pointer to a local variable will be invalid once the function returns. "
					  "Allocate the data on the heap or temp memory to return a pointer.");
	return false;
}

/**
 * We have the following possibilities:
 *  1. return
 *  2. return <non void expr>
 *  3. return <void expr>
 *
 * If we are in a block or a macro expansion we need to handle it differently.
 *
 * @param context
 * @param statement
 * @return
 */
static inline bool sema_analyse_return_stmt(SemaContext *context, Ast *statement)
{
	if (context->active_scope.in_defer)
	{
		SEMA_ERROR(statement, "Return is not allowed inside of a defer.");
		return false;
	}

	// This might be a return in a function block or a macro which must be treated differently.
	if (context->active_scope.flags & (SCOPE_EXPR_BLOCK | SCOPE_MACRO))
	{
		return sema_analyse_block_exit_stmt(context, statement);
	}
	// 1. We mark that the current scope ends with a jump.
	context->active_scope.jump_end = true;

	Type *expected_rtype = context->rtype;
	assert(expected_rtype && "We should always have known type from a function return.");

	Expr *return_expr = statement->return_stmt.expr;

	if (return_expr)
	{
		if (!sema_analyse_expr_rhs(context, expected_rtype, return_expr, type_is_optional(expected_rtype))) return false;
		if (!sema_check_not_stack_variable_escape(context, return_expr)) return false;
		if (!sema_return_optional_check_is_valid_in_scope(context, return_expr)) return false;
	}
	else
	{
		if (type_no_optional(expected_rtype)->canonical != type_void)
		{
			SEMA_ERROR(statement, "Expected to return a result of type %s.", type_to_error_string(expected_rtype));
			return false;
		}
		statement->return_stmt.cleanup = context_get_defers(context, context->active_scope.defer_last, 0, true);
		return true;
	}

	// Process any ensures.
	sema_inline_return_defers(context, statement, context->active_scope.defer_last, 0);
	if (context->call_env.ensures)
	{
		// Never generate an expression.
		if (return_expr && return_expr->expr_kind == EXPR_OPTIONAL) goto SKIP_ENSURE;
		AstId first = 0;
		AstId *append_id = &first;
		// Creating an assign statement
		AstId doc_directive = context->call_env.current_function->func_decl.docs;
		context->return_expr = return_expr;
		while (doc_directive)
		{
			Ast *directive = astptr(doc_directive);
			if (directive->contract_stmt.kind == CONTRACT_ENSURE)
			{
				bool success;
				SCOPE_START_WITH_FLAGS(SCOPE_ENSURE);
					success = assert_create_from_contract(context, directive, &append_id, statement->span);
				SCOPE_END;
				if (!success) return false;
			}
			doc_directive = directive->next;
		}
		if (!first) goto SKIP_ENSURE;
		if (statement->return_stmt.cleanup)
		{
			Ast *last = ast_last(astptr(statement->return_stmt.cleanup));
			last->next = first;
		}
		else
		{
			statement->return_stmt.cleanup = first;
		}
	}
SKIP_ENSURE:;

	assert(type_no_optional(statement->return_stmt.expr->type)->canonical == type_no_optional(expected_rtype)->canonical);
	return true;
}


static inline bool sema_analyse_try_unwrap(SemaContext *context, Expr *expr)
{
	assert(expr->expr_kind == EXPR_TRY_UNWRAP);
	Expr *ident = expr->try_unwrap_expr.variable;
	Expr *optional = expr->try_unwrap_expr.init;

	// Case A. Unwrapping a single variable.
	if (!optional)
	{
		if (!sema_analyse_expr(context, ident)) return false;
		// The `try foo()` case.
		if (ident->expr_kind != EXPR_IDENTIFIER)
		{
			expr->try_unwrap_expr.optional = ident;
			expr->try_unwrap_expr.lhs = NULL;
			expr->try_unwrap_expr.assign_existing = true;
			expr->type = type_bool;
			return true;
		}
		Decl *decl = ident->identifier_expr.decl;
		if (decl->decl_kind != DECL_VAR)
		{
			SEMA_ERROR(ident, "Expected this to be the name of an optional variable, but it isn't. Did you mistype?");
			return false;
		}
		if (!IS_OPTIONAL(decl))
		{
			if (decl->var.kind == VARDECL_UNWRAPPED)
			{
				SEMA_ERROR(ident, "This variable is already unwrapped, so you cannot use 'try' on it again, please remove the 'try'.");
				return false;
			}
			SEMA_ERROR(ident, "Expected this variable to be an optional, otherwise it can't be used for unwrap, maybe you didn't intend to use 'try'?");
			return false;
		}
		expr->try_unwrap_expr.decl = decl;
		expr->type = type_bool;
		sema_unwrap_var(context, decl);
		expr->resolve_status = RESOLVE_DONE;
		return true;
	}

	// Case B. We are unwrapping to a variable that may or may not exist.
	bool implicit_declaration = false;
	TypeInfo *var_type = expr->try_unwrap_expr.type;

	// 1. Check if we are doing an implicit declaration.
	if (!var_type && ident->expr_kind == EXPR_IDENTIFIER)
	{
		implicit_declaration = !sema_symbol_is_defined_in_scope(context, ident->identifier_expr.ident);
	}

	// 2. If we have a type for the variable, resolve it.
	if (var_type)
	{
		if (!sema_resolve_type_info(context, var_type)) return false;
		if (IS_OPTIONAL(var_type))
		{
			SEMA_ERROR(var_type, "Only non-optional types may be used as types for 'try', please remove the '!'.");
			return false;
		}
	}

	// 3. We interpret this as an assignment to an existing variable.
	if (!var_type && !implicit_declaration)
	{
		// 3a. Resolve the identifier.
		if (!sema_analyse_expr_lvalue(context, ident)) return false;

		// 3b. Make sure it's assignable
		if (!sema_expr_check_assign(context, ident)) return false;

		// 3c. It can't be optional either.
		if (IS_OPTIONAL(ident))
		{
			if (ident->expr_kind == EXPR_IDENTIFIER)
			{
				SEMA_ERROR(ident, "This is an optional variable, you should only have non-optional variables on the left side unless you use 'try' without '='.");
			}
			else
			{
				SEMA_ERROR(ident, "This is an optional expression, it can't go on the left hand side of a 'try'.");
			}
			return false;
		}

		// 3d. We can now analyse the expression using the variable type.
		if (!sema_analyse_expr(context, optional)) return false;

		if (!IS_OPTIONAL(optional))
		{
			SEMA_ERROR(optional, "Expected an optional expression to 'try' here. If it isn't an optional, remove 'try'.");
			return false;
		}

		if (!cast_implicit(context, optional, ident->type)) return false;

		expr->try_unwrap_expr.assign_existing = true;
		expr->try_unwrap_expr.lhs = ident;
	}
	else
	{
		// 4. We are creating a new variable

		// 4a. If we had a variable type, then our expression must be an identifier.
		if (ident->expr_kind != EXPR_IDENTIFIER)
		{
			SEMA_ERROR(ident, "A variable name was expected here.");
			return false;
		}

		assert(ident->resolve_status != RESOLVE_DONE);
		if (ident->identifier_expr.path)
		{
			SEMA_ERROR(ident->identifier_expr.path, "The variable may not have a path.");
			return false;
		}

		if (ident->identifier_expr.is_const)
		{
			SEMA_ERROR(ident, "Expected a variable starting with a lower case letter.");
			return false;
		}

		// 4b. Evaluate the expression
		if (!sema_analyse_expr(context, optional)) return false;

		if (!IS_OPTIONAL(optional))
		{
			SEMA_ERROR(optional, "Expected an optional expression to 'try' here. If it isn't an optional, remove 'try'.");
			return false;
		}

		if (var_type)
		{
			if (!cast_implicit(context, optional, var_type->type)) return false;
		}

		// 4c. Create a type_info if needed.
		if (!var_type)
		{
			var_type = type_info_new_base(optional->type->optional, optional->span);
		}

		// 4d. A new declaration is created.
		Decl *decl = decl_new_var(ident->identifier_expr.ident, ident->span, var_type, VARDECL_LOCAL);

		// 4e. Analyse it
		if (!sema_analyse_var_decl(context, decl, true)) return false;

		expr->try_unwrap_expr.decl = decl;
	}

	expr->try_unwrap_expr.optional = optional;
	expr->type = type_bool;
	expr->resolve_status = RESOLVE_DONE;
	return true;
}


static inline bool sema_analyse_try_unwrap_chain(SemaContext *context, Expr *expr, CondType cond_type)
{
	assert(cond_type == COND_TYPE_UNWRAP_BOOL || cond_type == COND_TYPE_UNWRAP);

	assert(expr->expr_kind == EXPR_TRY_UNWRAP_CHAIN);
	Expr **chain = expr->try_unwrap_chain_expr;

	VECEACH(expr->try_unwrap_chain_expr, i)
	{
		Expr *chain_element = chain[i];
		if (chain_element->expr_kind == EXPR_TRY_UNWRAP)
		{
			if (!sema_analyse_try_unwrap(context, chain_element)) return false;
			continue;
		}
		if (!sema_analyse_cond_expr(context, chain_element)) return false;
	}
	expr->type = type_bool;
	expr->resolve_status = RESOLVE_DONE;
	return true;
}
static inline bool sema_analyse_catch_unwrap(SemaContext *context, Expr *expr)
{
	Expr *ident = expr->catch_unwrap_expr.variable;

	bool implicit_declaration = false;
	TypeInfo *type = expr->catch_unwrap_expr.type;

	if (!type && !ident)
	{
		expr->catch_unwrap_expr.lhs = NULL;
		expr->catch_unwrap_expr.decl = NULL;
		goto RESOLVE_EXPRS;
	}
	if (!type && ident->expr_kind == EXPR_IDENTIFIER)
	{
		implicit_declaration = !sema_symbol_is_defined_in_scope(context, ident->identifier_expr.ident);
	}

	if (!type && !implicit_declaration)
	{
		if (!sema_analyse_expr_lvalue(context, ident)) return false;

		if (!sema_expr_check_assign(context, ident)) return false;

		if (ident->type->canonical != type_anyfault)
		{
			SEMA_ERROR(ident, "Expected the variable to have the type %s, not %s.", type_quoted_error_string(type_anyfault),
					   type_quoted_error_string(ident->type));
			return false;
		}

		expr->catch_unwrap_expr.lhs = ident;
		expr->catch_unwrap_expr.decl = NULL;
	}
	else
	{
		type = type ? type : type_info_new_base(type_anyfault, expr->span);

		if (!sema_resolve_type_info(context, type)) return false;

		if (type->type->canonical != type_anyfault)
		{
			SEMA_ERROR(type, "Expected the type to be %s, not %s.", type_quoted_error_string(type_anyfault),
					   type_quoted_error_string(type->type));
			return false;
		}
		if (ident->expr_kind != EXPR_IDENTIFIER)
		{
			SEMA_ERROR(ident, "A variable name was expected here.");
			return false;
		}

		assert(ident->resolve_status != RESOLVE_DONE);
		if (ident->identifier_expr.path)
		{
			SEMA_ERROR(ident->identifier_expr.path, "The variable may not have a path.");
			return false;
		}

		if (ident->identifier_expr.is_const)
		{
			SEMA_ERROR(ident, "Expected a variable starting with a lower case letter.");
			return false;
		}

		// 4d. A new declaration is created.
		Decl *decl = decl_new_var(ident->identifier_expr.ident, ident->span, type, VARDECL_LOCAL);
		decl->var.no_init = true;

		// 4e. Analyse it
		if (!sema_analyse_var_decl(context, decl, true)) return false;

		expr->catch_unwrap_expr.decl = decl;
		expr->catch_unwrap_expr.lhs = NULL;
	}
RESOLVE_EXPRS:;
	Expr **exprs = expr->catch_unwrap_expr.exprs;
	VECEACH(exprs, i)
	{
		Expr *fail = exprs[i];
		if (!sema_analyse_expr(context, fail)) return false;
		if (!type_is_optional(fail->type))
		{
			SEMA_ERROR(fail, "This expression is not optional, did you add it by mistake?");
			return false;
		}
	}
	expr->type = type_anyfault;
	expr->resolve_status = RESOLVE_DONE;
	return true;
}

static void sema_remove_unwraps_from_try(SemaContext *c, Expr *cond)
{
	assert(cond->expr_kind == EXPR_COND);
	Expr *last = VECLAST(cond->cond_expr);
	if (!last || last->expr_kind != EXPR_TRY_UNWRAP_CHAIN) return;
	Expr **chain = last->try_unwrap_chain_expr;
	VECEACH(chain, i)
	{
		Expr *expr = chain[i];
		if (expr->expr_kind != EXPR_TRY_UNWRAP) continue;
		if (expr->try_unwrap_expr.assign_existing) continue;
		if (expr->try_unwrap_expr.optional)
		{
			sema_erase_var(c, expr->try_unwrap_expr.decl);
		}
		else
		{
			sema_erase_unwrapped(c, expr->try_unwrap_expr.decl);
		}
	}
}

static inline bool sema_analyse_last_cond(SemaContext *context, Expr *expr, CondType cond_type)
{
	switch (expr->expr_kind)
	{
		case EXPR_TRY_UNWRAP_CHAIN:
			if (cond_type != COND_TYPE_UNWRAP_BOOL && cond_type != COND_TYPE_UNWRAP)
			{
				SEMA_ERROR(expr, "Try unwrapping is only allowed inside of a 'while' or 'if' conditional.");
				return false;
			}
			return sema_analyse_try_unwrap_chain(context, expr, cond_type);
		case EXPR_CATCH_UNWRAP:
			if (cond_type != COND_TYPE_UNWRAP_BOOL && cond_type != COND_TYPE_UNWRAP)
			{
				SEMA_ERROR(expr, "Catch unwrapping is only allowed inside of a 'while' or 'if' conditional, maybe '@catch(<expr>)' will do what you need?");
				return false;
			}
			return sema_analyse_catch_unwrap(context, expr);
		default:
			break;
	}

	if (cond_type != COND_TYPE_EVALTYPE_VALUE) goto NORMAL_EXPR;

	// Now we're analysing the last expression in a switch.
	// Case 1: switch (var = variant_expr)
	if (expr->expr_kind == EXPR_BINARY && expr->binary_expr.operator == BINARYOP_ASSIGN)
	{
		// No variable on the lhs? Then it can't be an any unwrap.
		Expr *left = exprptr(expr->binary_expr.left);
		if (left->resolve_status ==  RESOLVE_DONE || left->expr_kind != EXPR_IDENTIFIER || left->identifier_expr.path) goto NORMAL_EXPR;

		// Does the identifier exist in the parent scope?
		// then again it can't be an any unwrap.
		if (sema_symbol_is_defined_in_scope(context, left->identifier_expr.ident)) goto NORMAL_EXPR;

		Expr *right = exprptr(expr->binary_expr.right);
		bool is_deref = right->expr_kind == EXPR_UNARY && right->unary_expr.operator == UNARYOP_DEREF;
		if (is_deref) right = right->unary_expr.expr;
		if (!sema_analyse_expr_rhs(context, NULL, right, false)) return false;
		Type *type = right->type->canonical;
		if (type == type_get_ptr(type_any) && is_deref)
		{
			is_deref = false;
			right = exprptr(expr->binary_expr.right);
			if (!sema_analyse_expr_rhs(context, NULL, right, false)) return false;
		}
		if (type != type_any) goto NORMAL_EXPR;
		// Found an expansion here
		expr->expr_kind = EXPR_ANYSWITCH;
		expr->any_switch.new_ident = left->identifier_expr.ident;
		expr->any_switch.span = left->span;
		expr->any_switch.any_expr = right;
		expr->any_switch.is_deref = is_deref;
		expr->any_switch.is_assign = true;
		expr->resolve_status = RESOLVE_DONE;
		expr->type = type_typeid;
		return true;
	}
	if (!sema_analyse_expr(context, expr)) return false;
	Type *type = expr->type->canonical;
	if (type != type_any) return true;
	if (expr->expr_kind == EXPR_IDENTIFIER)
	{
		Decl *decl = expr->identifier_expr.decl;
		expr->expr_kind = EXPR_ANYSWITCH;
		expr->any_switch.is_deref = false;
		expr->any_switch.is_assign = false;
		expr->any_switch.variable = decl;
		expr->type = type_typeid;
		expr->resolve_status = RESOLVE_DONE;
		return true;
	}
	return true;

NORMAL_EXPR:
	return sema_analyse_expr(context, expr);
}
/**
 * An decl-expr-list is a list of a mixture of declarations and expressions.
 * The last declaration or expression is propagated. So for example:
 *
 *   int a = 3, b = 4, float c = 4.0
 *
 * In this case the final value is 4.0 and the type is float.
 */
static inline bool sema_analyse_cond_list(SemaContext *context, Expr *expr, CondType cond_type)
{
	assert(expr->expr_kind == EXPR_COND);

	Expr **dexprs = expr->cond_expr;
	unsigned entries = vec_size(dexprs);

	// 1. Special case, there are no entries, so the type is void
	if (entries == 0)
	{
		expr->type = type_void;
		return true;
	}

	// 2. Walk through each of our declarations / expressions as if they were regular expressions.
	for (unsigned i = 0; i < entries - 1; i++)
	{
		if (!sema_analyse_expr(context, dexprs[i])) return false;
	}

	if (!sema_analyse_last_cond(context, dexprs[entries - 1], cond_type)) return false;

	expr->type = dexprs[entries - 1]->type;
	expr->resolve_status = RESOLVE_DONE;
	return true;
}


/**
 * Analyse a conditional expression:
 *
 *  1. The middle statement in a for loop
 *  2. The expression in an if, while
 *  3. The expression in a switch
 *
 * @param context the current context
 * @param expr the conditional to evaluate
 * @param cast_to_bool if the result is to be cast to bool after
 * @return true if it passes analysis.
 */
static inline bool sema_analyse_cond(SemaContext *context, Expr *expr, CondType cond_type)
{
	bool cast_to_bool = cond_type == COND_TYPE_UNWRAP_BOOL;
	assert(expr->expr_kind == EXPR_COND && "Conditional expressions should always be of type EXPR_DECL_LIST");

	// 1. Analyse the declaration list.
	if (!sema_analyse_cond_list(context, expr, cond_type)) return false;

	// 2. If we get "void", either through a void call or an empty list,
	//    signal that.
	if (type_is_void(expr->type))
	{
		SEMA_ERROR(expr, cast_to_bool ? "Expected a boolean expression." : "Expected an expression resulting in a value.");
		return false;
	}

	// 3. We look at the last element (which is guaranteed to exist because
	//    the type was not void.
	Expr *last = VECLAST(expr->cond_expr);

	if (last->expr_kind == EXPR_DECL)
	{
		// 3c. The declaration case
		Decl *decl = last->decl_expr;
		Expr *init = decl->var.init_expr;
		// 3d. We expect an initialization for the last declaration.
		if (!init)
		{
			SEMA_ERROR(last, "Expected a declaration with initializer.");
			return false;
		}
		// 3e. Expect that it isn't an optional
		if (IS_OPTIONAL(init) && !decl->var.unwrap)
		{
			return sema_error_failed_cast(last, last->type, cast_to_bool ? type_bool : init->type);
			return false;
		}
		// TODO document
		if (!decl->var.unwrap && cast_to_bool && cast_to_bool_kind(typeget(decl->var.type_info)) == CAST_ERROR)
		{
			SEMA_ERROR(last->decl_expr->var.init_expr, "The expression needs to be convertible to a boolean.");
			return false;
		}
		return true;
	}

	// 3a. Check for optional in case of an expression.
	if (IS_OPTIONAL(last))
	{
		if (type_no_optional(last->type) == type_void && cast_to_bool)
		{
			SEMA_ERROR(last, "Use '@ok(<expr>)' or '@catch(<expr>)' to explicitly convert a 'void!' to a boolean.");
			return false;
		}
		SEMA_ERROR(last, "The expression may not be an optional, but was %s.", type_quoted_error_string(last->type));
		return false;
	}
	// 3b. Cast to bool if that is needed
	if (cast_to_bool)
	{
		if (!cast_explicit(context, last, type_bool)) return false;
	}
	return true;
}


static inline bool sema_analyse_decls_stmt(SemaContext *context, Ast *statement)
{
	bool should_nop = true;
	FOREACH_BEGIN_IDX(i, Decl *decl, statement->decls_stmt)
		VarDeclKind kind = decl->var.kind;
		if (kind == VARDECL_LOCAL_CT_TYPE || kind == VARDECL_LOCAL_CT)
		{
			if (!sema_analyse_var_decl_ct(context, decl)) return false;
			statement->decls_stmt[i] = NULL;
		}
		else
		{
			if (!sema_analyse_var_decl(context, decl, true)) return false;
			should_nop = false;
		}
	FOREACH_END();
	if (should_nop) statement->ast_kind = AST_NOP_STMT;
	return true;
}

static inline bool sema_analyse_declare_stmt(SemaContext *context, Ast *statement)
{
	VarDeclKind kind = statement->declare_stmt->var.kind;
	bool erase = kind == VARDECL_LOCAL_CT_TYPE || kind == VARDECL_LOCAL_CT;
	if (!sema_analyse_var_decl(context, statement->declare_stmt, true)) return false;
	if (erase) statement->ast_kind = AST_NOP_STMT;
	return true;
}

static inline bool sema_analyse_expr_stmt(SemaContext *context, Ast *statement)
{
	Expr *expr = statement->expr_stmt;
	if (!sema_analyse_expr(context, expr)) return false;
	if (!sema_expr_check_discard(expr)) return false;
	switch (expr->expr_kind)
	{
		case EXPR_CALL:
			if (expr->call_expr.no_return) context->active_scope.jump_end = true;
			break;
		case EXPR_MACRO_BLOCK:
			if (expr->macro_block.is_noreturn) context->active_scope.jump_end = true;
			break;
		case EXPR_CONST:
			// Remove all const statements.
			statement->ast_kind = AST_NOP_STMT;
			break;
		default:
			break;
	}
	return true;
}

bool sema_analyse_defer_stmt_body(SemaContext *context, Ast *statement, Ast *body)
{
	if (body->ast_kind == AST_DEFER_STMT)
	{
		SEMA_ERROR(body, "A defer may not have a body consisting of a raw 'defer', this looks like a mistake.");
		return false;
	}
	bool success;
	SCOPE_START

	context->active_scope.defer_last = 0;
	context->active_scope.defer_start = 0;
	context->active_scope.in_defer = statement;

	PUSH_BREAKCONT(NULL);
	PUSH_NEXT(NULL, NULL);

	// Only ones allowed.
	context->active_scope.flags = 0;

	success = sema_analyse_statement(context, body);

	POP_BREAKCONT();
	POP_NEXT();

	// We should never need to replace any defers here.

	SCOPE_END;

	return success;

}
static inline bool sema_analyse_defer_stmt(SemaContext *context, Ast *statement)
{
	if (!sema_analyse_defer_stmt_body(context, statement, astptr(statement->defer_stmt.body))) return false;

	statement->defer_stmt.prev_defer = context->active_scope.defer_last;
	context->active_scope.defer_last = astid(statement);

	return true;
}

static inline bool sema_analyse_for_cond(SemaContext *context, ExprId *cond_ref, bool *infinite)
{
	ExprId cond_id = *cond_ref;
	if (!cond_id)
	{
		*infinite = true;
		return true;
	}
	Expr *cond = exprptr(cond_id);
	if (cond->expr_kind == EXPR_COND)
	{
		if (!sema_analyse_cond(context, cond, COND_TYPE_UNWRAP_BOOL)) return false;
	}
	else
	{
		if (!sema_analyse_cond_expr(context, cond)) return false;
	}

	// If this is const true, then set this to infinite and remove the expression.
	Expr *cond_last = cond->expr_kind == EXPR_COND ? VECLAST(cond->cond_expr) : cond;
	assert(cond_last);
	if (expr_is_const(cond_last) && cond_last->const_expr.b)
	{
		if (cond->expr_kind != EXPR_COND || vec_size(cond->cond_expr) == 1)
		{
			cond = NULL;
		}
		*infinite = true;
	}
	else
	{
		*infinite = false;
	}
	*cond_ref = cond ? exprid(cond) : 0;
	return true;
}
static inline bool sema_analyse_for_stmt(SemaContext *context, Ast *statement)
{
	bool success = true;
	bool is_infinite = false;

	Ast *body = astptr(statement->for_stmt.body);
	assert(body);
	if (body->ast_kind == AST_DEFER_STMT)
	{
		SEMA_ERROR(body, "Looping over a raw 'defer' is not allowed, was this a mistake?");
		return false;
	}
	bool do_loop = statement->for_stmt.flow.skip_first;
	if (body->ast_kind != AST_COMPOUND_STMT && do_loop)
	{
		SEMA_ERROR(body, "A do loop must use { } around its body.");
		return false;
	}
	// Enter for scope
	SCOPE_OUTER_START

		if (statement->for_stmt.init)
		{
			success = sema_analyse_expr(context, exprptr(statement->for_stmt.init));
		}

		// Conditional scope start
		SCOPE_START_WITH_LABEL(statement->for_stmt.flow.label)

			if (!do_loop)
			{
				if (!sema_analyse_for_cond(context, &statement->for_stmt.cond, &is_infinite) || !success)
				{
					SCOPE_ERROR_END_OUTER();
					return false;
				}
			}


			PUSH_BREAKCONT(statement);
				success = sema_analyse_statement(context, body);
				statement->for_stmt.flow.no_exit = context->active_scope.jump_end;
			POP_BREAKCONT();

			// End for body scope
			context_pop_defers_and_replace_ast(context, body);

		SCOPE_END;

		if (statement->for_stmt.flow.skip_first)
		{
			SCOPE_START
				if (!sema_analyse_for_cond(context, &statement->for_stmt.cond, &is_infinite) || !success)
				{
					SCOPE_ERROR_END_OUTER();
					return false;
				}
			SCOPE_END;
			// Rewrite do { } while(true) to while(true) { }
			if (is_infinite)
			{
				assert(!statement->for_stmt.cond);
				statement->for_stmt.flow.skip_first = false;
			}
		}

		if (success && statement->for_stmt.incr)
		{
			// Incr scope start
			SCOPE_START
				success = sema_analyse_expr(context, exprptr(statement->for_stmt.incr));
				// Incr scope end
			SCOPE_END;
		}


		// End for body scope
		context_pop_defers_and_replace_ast(context, statement);

	SCOPE_OUTER_END;

	if (is_infinite && !statement->for_stmt.flow.has_break)
	{
		context->active_scope.jump_end = true;
	}
	return success;
}

/**
 * foreach_stmt ::= foreach
 * @param context
 * @param statement
 * @return
 */
static inline bool sema_analyse_foreach_stmt(SemaContext *context, Ast *statement)
{
	// Pull out the relevant data.
	Decl *var = declptr(statement->foreach_stmt.variable);
	Decl *index = declptrzero(statement->foreach_stmt.index);
	Expr *enumerator = exprptr(statement->foreach_stmt.enumeration);
	AstId body = statement->foreach_stmt.body;
	AstId first_stmt = 0;
	AstId *succ = &first_stmt;
	Expr **expressions = NULL;
	bool is_reverse = statement->foreach_stmt.is_reverse;
	bool value_by_ref = statement->foreach_stmt.value_by_ref;
	bool success = true;

	// First fold the enumerator expression, removing any () around it.
	while (enumerator->expr_kind == EXPR_GROUP) enumerator = enumerator->inner_expr;

	bool iterator_based = false;

	// Conditional scope start
	SCOPE_START

		// In the case of foreach (int x : { 1, 2, 3 }) we will infer the int[] type, so pick out the number of elements.
		Type *inferred_type = NULL;

		// We may have an initializer list, in this case we rely on an inferred type.
		if (expr_is_init_list(enumerator) || expr_is_const_initializer(enumerator))
		{
			bool may_be_array;
			bool is_const_size;
			MemberIndex size = sema_get_initializer_const_array_size(context, enumerator, &may_be_array, &is_const_size);
			if (!may_be_array)
			{
				SEMA_ERROR(enumerator,
						   "This initializer appears to be a struct initializer when, an array initializer was expected.");
				return SCOPE_POP_ERROR();
			}
			if (!is_const_size)
			{
				SEMA_ERROR(enumerator, "Only constant sized initializers may be implicitly initialized.");
				return SCOPE_POP_ERROR();
			}
			if (size < 0)
			{
				SEMA_ERROR(enumerator, "The initializer mixes designated initialization with array initialization.");
				return SCOPE_POP_ERROR();
			}
			assert(size >= 0);

			TypeInfo *variable_type_info = vartype(var);

			if (!variable_type_info)
			{
				SEMA_ERROR(var, "Add the type of your variable here if you want to iterate over an initializer list.");
				return SCOPE_POP_ERROR();
			}
			// First infer the type of the variable.
			if (!sema_resolve_type_info(context, variable_type_info)) return false;
			// And create the inferred type:
			inferred_type = type_get_array(variable_type_info->type, (ArraySize)size);
		}

		// because we don't want the index + variable to move into the internal scope
		if (!sema_analyse_inferred_expr(context, inferred_type, enumerator))
		{
			// Exit early here, because semantic checking might be messed up otherwise.
			return SCOPE_POP_ERROR();
		}

		// And pop the cond scope.
	SCOPE_END;

	if (IS_OPTIONAL(enumerator))
	{
		SEMA_ERROR(enumerator, "The expression may not be optional.");
		return false;
	}

	if (statement->foreach_stmt.index_by_ref)
	{
		assert(index);
		SEMA_ERROR(index, "The index cannot be held by reference, did you accidentally add a '&'?");
		return false;
	}

	// Insert a single deref as needed.
	Type *flattened_type = enumerator->type->canonical;
	if (flattened_type->type_kind == TYPE_POINTER)
	{
		// Something like Foo** will not be dereferenced, only Foo*
		if (flattened_type->pointer->type_kind == TYPE_POINTER)
		{
			SEMA_ERROR(enumerator, "It is not possible to enumerate an expression of type %s.", type_quoted_error_string(enumerator->type));
			return false;
		}
		expr_rewrite_insert_deref(enumerator);
	}

	// At this point we should have dereferenced any pointer or bailed.
	assert(!type_is_pointer(enumerator->type));

	// Check that we can even index this expression.

	Type *value_type = type_get_indexed_type(enumerator->type);
	if (value_type && value_by_ref) value_type = type_get_ptr(value_type);

	Decl *len = NULL;
	Decl *index_macro = NULL;
	Type *index_type = type_usz;

	if (!value_type)
	{
		len = sema_find_operator(context, enumerator, OVERLOAD_LEN);
		Decl *by_val = sema_find_operator(context, enumerator, OVERLOAD_ELEMENT_AT);
		Decl *by_ref = sema_find_operator(context, enumerator, OVERLOAD_ELEMENT_REF);
		if (!len || (!by_val && !by_ref))
		{
			SEMA_ERROR(enumerator, "It's not possible to enumerate an expression of type %s.", type_quoted_error_string(enumerator->type));
			return false;
		}
		if (!by_ref && value_by_ref)
		{
			SEMA_ERROR(enumerator, "%s does not support 'foreach' with the value by reference.", type_quoted_error_string(enumerator->type));
			return false;
		}
		index_macro = value_by_ref ? by_ref : by_val;
		assert(index_macro);
		index_type = index_macro->func_decl.signature.params[1]->type;
		if (!type_is_integer(index_type))
		{
			SEMA_ERROR(enumerator, "Only integer indexed types may be used with foreach.");
			return false;
		}
		TypeInfoId rtype = index_macro->func_decl.signature.rtype;
		value_type = rtype ? type_infoptr(rtype)->type : NULL;
	}


	TypeInfo *type_info = vartype(var);
	// Set up the value, assigning the type as needed.
	// Element *value @noinit
	if (!type_info)
	{
		type_info = type_info_new_base(value_type, var->span);
		var->var.type_info = type_infoid(type_info);
	}
	if (!sema_resolve_type_info(context, type_info)) return false;

	if (type_is_optional(type_info->type))
	{
		SEMA_ERROR(type_info, "The variable may not be an optional.");
		return false;
	}

	// Set up the optional index parameter
	Type *index_var_type = NULL;
	if (index)
	{
		TypeInfo *idx_type_info = vartype(index);
		if (!idx_type_info)
		{
			idx_type_info = type_info_new_base(index_type, enumerator->span);
			index->var.type_info = type_infoid(idx_type_info);
		}
		if (!sema_resolve_type_info(context, idx_type_info)) return false;
		index_var_type = idx_type_info->type;
		if (type_is_optional(index_var_type))
		{
			SEMA_ERROR(idx_type_info, "The index may not be an optional.");
			return false;
		}
		if (!type_is_integer(type_flatten(index_var_type)))
		{
			SEMA_ERROR(idx_type_info,
					   "Index must be an integer type, '%s' is not valid.",
					   type_to_error_string(index_var_type));
			return false;
		}
	}


	// We either have "foreach (x : some_var)" or "foreach (x : some_call())"
	// So we grab the former by address (implicit &) and the latter as the value.
	assert(enumerator->resolve_status == RESOLVE_DONE);
	bool is_addr = false;
	bool is_variable = false;
	if (enumerator->expr_kind == EXPR_IDENTIFIER)
	{
		enumerator->identifier_expr.decl->var.is_written = true;
		is_variable = true;
	}
	else if (expr_may_addr(enumerator))
	{
		is_addr = true;
		expr_insert_addr(enumerator);
	}

	Decl *temp = NULL;
	if (is_variable)
	{
		temp = enumerator->identifier_expr.decl;
	}
	else
	{
		// Store either "Foo* __enum$ = &some_var;" or "Foo __enum$ = some_call()"
		temp = decl_new_generated_var(enumerator->type, VARDECL_LOCAL, enumerator->span);
		vec_add(expressions, expr_generate_decl(temp, enumerator));
	}

	// Create @__enum$.len() or @(*__enum$).len()
	Expr *enum_val = expr_variable(temp);
	if (is_addr) expr_rewrite_insert_deref(enum_val);
	Type *enumerator_type = type_flatten(enum_val->type);
	Expr *len_call;
	ArraySize array_len = 0;
	if (len)
	{
		len_call = expr_new(EXPR_CALL, enumerator->span);
		if (!sema_insert_method_call(context, len_call, len, enum_val, NULL)) return false;
	}
	else
	{
		if (enumerator_type->type_kind == TYPE_ARRAY)
		{
			array_len = enumerator_type->array.len;
			len_call = NULL;
		}
		else
		{
			len_call = expr_new(EXPR_BUILTIN_ACCESS, enumerator->span);
			if (!sema_analyse_expr(context, enum_val)) return false;
			len_call->builtin_access_expr.inner = exprid(enum_val);
			len_call->builtin_access_expr.kind = ACCESS_LEN;
			len_call->resolve_status = RESOLVE_DONE;
			len_call->type = type_isz;
		}
	}
	Decl *idx_decl = decl_new_generated_var(index_type, VARDECL_LOCAL, index ? index->span : enumerator->span);

	// IndexType __len$ = (IndexType)(@__enum$.len())
	Decl *len_decl = NULL;

	if (is_reverse)
	{
		if (!len_call)
		{
			// Create const len if missing.
			len_call = expr_new_const_int(enumerator->span, type_isz, array_len);
		}
		if (!cast_implicit(context, len_call, index_type)) return false;
		// __idx$ = (IndexType)(@__enum$.len()) (or const)
		vec_add(expressions, expr_generate_decl(idx_decl, len_call));
	}
	else
	{
		if (len_call)
		{
			len_decl = decl_new_generated_var(index_type, VARDECL_LOCAL, enumerator->span);
			if (!cast_implicit(context, len_call, index_type)) return false;
			vec_add(expressions, expr_generate_decl(len_decl, len_call));
		}
		Expr *idx_init = expr_new_const_int(idx_decl->span, index_type, 0);
		vec_add(expressions, expr_generate_decl(idx_decl, idx_init));
	}

	// Add all declarations to the init
	Expr *init_expr = expr_new(EXPR_EXPRESSION_LIST, var->span);
	init_expr->expression_list = expressions;

	Expr *update = NULL;
	Expr *cond;
	if (is_reverse)
	{
		// Create __idx$ > 0
		cond = expr_new(EXPR_BINARY, idx_decl->span);
		cond->binary_expr.operator = BINARYOP_GT;
		cond->binary_expr.left = exprid(expr_variable(idx_decl));
		Expr *rhs = expr_new_const_int(enumerator->span, index_type, 0);
		cond->binary_expr.right = exprid(rhs);

		// Create --__idx$
		Expr *dec = expr_new(EXPR_UNARY, idx_decl->span);
		dec->unary_expr.expr = expr_variable(idx_decl);
		dec->unary_expr.operator = UNARYOP_DEC;
		Ast *update_stmt = new_ast(AST_EXPR_STMT, idx_decl->span);
		update_stmt->expr_stmt = dec;
		ast_append(&succ, update_stmt);
	}
	else
	{
		// Create __idx$ < __len$
		cond = expr_new(EXPR_BINARY, idx_decl->span);
		cond->binary_expr.operator = BINARYOP_LT;
		cond->binary_expr.left = exprid(expr_variable(idx_decl));
		if (len_decl)
		{
			cond->binary_expr.right = exprid(expr_variable(len_decl));
		}
		else
		{
			Expr *rhs = expr_new_const_int(enumerator->span, type_isz, array_len);
			cond->binary_expr.right = exprid(rhs);
		}

		// Create ++__idx$
		update = expr_new(EXPR_UNARY, idx_decl->span);
		update->unary_expr.expr = expr_variable(idx_decl);
		update->unary_expr.operator = UNARYOP_INC;
	}

	// Create IndexType index = __idx$
	if (index)
	{
		Ast *declare_ast = new_ast(AST_DECLARE_STMT, var->span);
		declare_ast->declare_stmt = index;
		Expr *load_idx = expr_variable(idx_decl);
		if (!cast_explicit(context, load_idx, index_var_type)) return false;
		index->var.init_expr = load_idx;
		ast_append(&succ, declare_ast);
	}

	// Create value = (*__$enum)[__idx$]
	Ast *value_declare_ast = new_ast(AST_DECLARE_STMT, var->span);
	value_declare_ast->declare_stmt = var;

	Expr *subscript = expr_new(EXPR_SUBSCRIPT, var->span);
	enum_val = expr_variable(temp);
	if (is_addr) expr_rewrite_insert_deref(enum_val);
	subscript->subscript_expr.expr = exprid(enum_val);
	subscript->subscript_expr.range.start = exprid(expr_variable(idx_decl));
	if (value_by_ref)
	{
		Expr *addr = expr_new(EXPR_UNARY, subscript->span);
		addr->unary_expr.operator = UNARYOP_ADDR;
		addr->unary_expr.expr = subscript;
		subscript = addr;
	}
	var->var.init_expr = subscript;
	ast_append(&succ, value_declare_ast);
	Ast *body_ast = astptr(body);
	ast_append(&succ, body_ast);
	Ast *compound_stmt = new_ast(AST_COMPOUND_STMT, body_ast->span);
	compound_stmt->compound_stmt.first_stmt = first_stmt;
	FlowCommon flow = statement->foreach_stmt.flow;
	statement->for_stmt = (AstForStmt){ .init = exprid(init_expr),
										.cond = exprid(cond),
										.incr = update ? exprid(update) : 0,
										.flow = flow,
										.body = astid(compound_stmt)
	};
	statement->ast_kind = AST_FOR_STMT;
	return sema_analyse_for_stmt(context, statement);

}

static inline bool sema_analyse_if_stmt(SemaContext *context, Ast *statement)
{
	// IMPROVE
	// convert
	// if (!x) A(); else B();
	// into
	// if (x) B(); else A();

	bool else_jump;
	bool then_jump;
	bool success;

	Expr *cond = exprptr(statement->if_stmt.cond);
	Ast *then = astptr(statement->if_stmt.then_body);
	if (then->ast_kind == AST_DEFER_STMT)
	{
		SEMA_ERROR(then, "An 'if' statement may not be followed by a raw 'defer' statement, this looks like a mistake.");
		return false;
	}
	AstId else_id = statement->if_stmt.else_body;
	Ast *else_body = else_id ? astptr(else_id) : NULL;
	SCOPE_OUTER_START
		CondType cond_type = then->ast_kind == AST_IF_CATCH_SWITCH_STMT
							 ? COND_TYPE_UNWRAP : COND_TYPE_UNWRAP_BOOL;
		success = sema_analyse_cond(context, cond, cond_type);

		if (success && !ast_ok(then))
		{
			SEMA_ERROR(then,
					   "The 'then' part of a single line if-statement must start on the same line as the 'if' or use '{ }'");
			success = false;
		}

		if (success && else_body)
		{
			bool then_has_braces = then->ast_kind == AST_COMPOUND_STMT || then->ast_kind == AST_IF_CATCH_SWITCH_STMT;
			if (!then_has_braces)
			{
				SEMA_ERROR(then, "if-statements with an 'else' must use '{ }' even around a single statement.");
				success = false;
			}
			if (success && else_body->ast_kind != AST_COMPOUND_STMT &&
				else_body->ast_kind != AST_IF_STMT)
			{
				SEMA_ERROR(else_body,
						   "An 'else' must use '{ }' even around a single statement.");
				success = false;
			}
		}
		if (context->active_scope.jump_end && !context->active_scope.allow_dead_code)
		{
			SEMA_ERROR(then, "This code can never be executed.");
			success = false;
		}

		if (then->ast_kind == AST_IF_CATCH_SWITCH_STMT)
		{
			DeclId label_id = statement->if_stmt.flow.label;
			then->switch_stmt.flow.label = label_id;
			statement->if_stmt.flow.label = 0;
			Decl *label = declptrzero(label_id);
			if (label) label->label.parent = astid(then);
			SCOPE_START_WITH_LABEL(label_id);
				success = success && sema_analyse_switch_stmt(context, then);
				then_jump = context->active_scope.jump_end;
			SCOPE_END;
		}
		else
		{
			SCOPE_START_WITH_LABEL(statement->if_stmt.flow.label);
				success = success && sema_analyse_statement(context, then);
				then_jump = context->active_scope.jump_end;
			SCOPE_END;
		}

		if (!success) goto END;
		else_jump = false;
		if (statement->if_stmt.else_body)
		{
			SCOPE_START_WITH_LABEL(statement->if_stmt.flow.label);
				sema_remove_unwraps_from_try(context, cond);
				sema_unwrappable_from_catch_in_else(context, cond);
				success = success && sema_analyse_statement(context, else_body);
				else_jump = context->active_scope.jump_end;
			SCOPE_END;
		}

END:
		context_pop_defers_and_replace_ast(context, statement);

	SCOPE_OUTER_END;
	if (!success) return false;
	if (then_jump)
	{
		sema_unwrappable_from_catch_in_else(context, cond);
	}
	if (then_jump && else_jump && !statement->flow.has_break)
	{
		context->active_scope.jump_end = true;
	}
	return true;
}

static bool sema_analyse_asm_string_stmt(SemaContext *context, Ast *stmt)
{
	Expr *body = exprptr(stmt->asm_block_stmt.asm_string);
	if (!sema_analyse_expr(context, body)) return false;
	if (!expr_is_const_string(body))
	{
		SEMA_ERROR(body, "The asm statement expects a constant string.");
		return false;
	}
	return true;
}


/**
 * When jumping to a label
 * @param context
 * @param stmt
 * @return
 */
static inline Decl *sema_analyse_label(SemaContext *context, Ast *stmt)
{
	Label *label = stmt->ast_kind == AST_NEXTCASE_STMT ? &stmt->nextcase_stmt.label : &stmt->contbreak_stmt.label;
	const char *name = label->name;
	Decl *target = sema_find_label_symbol(context, name);
	if (!target)
	{
		target = sema_find_label_symbol_anywhere(context, name);
		if (target && target->decl_kind == DECL_LABEL)
		{
			if (context->active_scope.flags & SCOPE_EXPR_BLOCK)
			{
				switch (stmt->ast_kind)
				{
					case AST_BREAK_STMT:
						SEMA_ERROR(stmt, "You cannot break out of an expression block.");
						return poisoned_decl;
					case AST_CONTINUE_STMT:
						SEMA_ERROR(stmt, "You cannot use continue out of an expression block.");
						return poisoned_decl;
					case AST_NEXTCASE_STMT:
						SEMA_ERROR(stmt, "You cannot use nextcase to exit an expression block.");
						return poisoned_decl;
					default:
						UNREACHABLE
				}
			}
			if (target->label.scope_defer != astid(context->active_scope.in_defer))
			{
				switch (stmt->ast_kind)
				{
					case AST_BREAK_STMT:
						SEMA_ERROR(stmt, "You cannot break out of a defer.");
						return poisoned_decl;
					case AST_CONTINUE_STMT:
						SEMA_ERROR(stmt, "You cannot use continue out of a defer.");
						return poisoned_decl;
					case AST_NEXTCASE_STMT:
						SEMA_ERROR(stmt, "You cannot use nextcase out of a defer.");
						return poisoned_decl;
					default:
						UNREACHABLE
				}
			}
			SEMA_ERROR(stmt, "'%s' cannot be reached from the current scope.", name);
			return poisoned_decl;
		}
		SEMA_ERROR(stmt, "A labelled statement with the name '%s' can't be found in the current scope.", name);
		return poisoned_decl;
	}
	if (target->decl_kind != DECL_LABEL)
	{
		SEMA_ERROR(label, "Expected the name to match a label, not a constant.");
		return poisoned_decl;
	}
	if (context->active_scope.in_defer)
	{
		if (target->label.scope_defer != astid(context->active_scope.in_defer))
		{
			switch (stmt->ast_kind)
			{
				case AST_BREAK_STMT:
					SEMA_ERROR(stmt, "You cannot break out of a defer.");
					return poisoned_decl;
				case AST_CONTINUE_STMT:
					SEMA_ERROR(stmt, "You cannot use continue out of a defer.");
					return poisoned_decl;
				case AST_NEXTCASE_STMT:
					SEMA_ERROR(stmt, "You cannot use nextcase out of a defer.");
					return poisoned_decl;
				default:
					UNREACHABLE
			}
		}
	}
	return target;
}

static bool context_labels_exist_in_scope(SemaContext *context)
{
	Decl **locals = context->locals;
	for (size_t local = context->active_scope.current_local; local > 0; local--)
	{
		if (locals[local - 1]->decl_kind == DECL_LABEL) return true;
	}
	return false;
}


static bool sema_analyse_nextcase_stmt(SemaContext *context, Ast *statement)
{
	context->active_scope.jump_end = true;
	if (!context->next_target && !statement->nextcase_stmt.label.name && !statement->nextcase_stmt.expr && !statement->nextcase_stmt.is_default)
	{
		if (context->next_switch)
		{
			SEMA_ERROR(statement, "A plain 'nextcase' is not allowed on the last case.");
		}
		else
		{
			SEMA_ERROR(statement, "'nextcase' can only be used inside of a switch.");
		}
		return false;
	}

	// TODO test that nextcase out of a switch using label correctly creates defers.
	Ast *parent = context->next_switch;
	if (statement->nextcase_stmt.label.name)
	{
		Decl *target = sema_analyse_label(context, statement);
		if (!decl_ok(target)) return false;
		parent = astptr(target->label.parent);
		AstKind kind = parent->ast_kind;
		if (kind != AST_SWITCH_STMT && kind != AST_IF_CATCH_SWITCH_STMT)
		{
			SEMA_ERROR(&statement->nextcase_stmt.label, "Expected the label to match a 'switch' or 'if-catch' statement.");
			return false;
		}
	}
	if (!parent)
	{
		SEMA_ERROR(statement, "No matching switch could be found.");
		return false;
	}

	// Handle jump to default.
	Ast **cases = parent->switch_stmt.cases;
	if (statement->nextcase_stmt.is_default)
	{
		Ast *default_ast = NULL;
		FOREACH_BEGIN(Ast *cs, cases)
			if (cs->ast_kind == AST_DEFAULT_STMT)
			{
				default_ast = cs;
				break;
			}
		FOREACH_END();
		if (!default_ast) RETURN_SEMA_ERROR(statement, "There is no 'default' in the switch to jump to.");
		statement->nextcase_stmt.defer_id = context_get_defers(context, context->active_scope.defer_last, parent->switch_stmt.defer, true);
		statement->nextcase_stmt.case_switch_stmt = astid(default_ast);
		statement->nextcase_stmt.switch_expr = NULL;
		return true;
	}

	Expr *value = exprptrzero(statement->nextcase_stmt.expr);
	statement->nextcase_stmt.switch_expr = NULL;
	if (!value)
	{
		assert(context->next_target);
		statement->nextcase_stmt.defer_id = context_get_defers(context, context->active_scope.defer_last, parent->switch_stmt.defer, true);
		statement->nextcase_stmt.case_switch_stmt = astid(context->next_target);
		return true;
	}

	Expr *cond = exprptrzero(parent->switch_stmt.cond);

	if (!cond)
	{
		RETURN_SEMA_ERROR(statement, "'nextcase' cannot be used with an expressionless switch.");
	}

	if (value->expr_kind == EXPR_TYPEINFO)
	{
		TypeInfo *type_info = value->type_expr;
		if (!sema_resolve_type_info(context, type_info)) return false;
		statement->nextcase_stmt.defer_id = context_get_defers(context, context->active_scope.defer_last, parent->switch_stmt.defer, true);
		if (cond->type->canonical != type_typeid)
		{
			SEMA_ERROR(statement, "Unexpected 'type' in as an 'nextcase' destination.");
			SEMA_NOTE(statement, "The 'switch' here uses expected a type '%s'.", type_to_error_string(cond->type));
			return false;
		}
		cases = parent->switch_stmt.cases;

		Type *type = type_info->type->canonical;
		VECEACH(cases, i)
		{
			Ast *case_stmt = cases[i];
			if (case_stmt->ast_kind == AST_DEFAULT_STMT) continue;
			Expr *expr = exprptr(case_stmt->case_stmt.expr);
			if (expr_is_const(expr) && expr->const_expr.typeid == type)
			{
				statement->nextcase_stmt.case_switch_stmt = astid(case_stmt);
				return true;
			}
		}
		SEMA_ERROR(type_info, "There is no case for type '%s'.", type_to_error_string(type_info->type));
		return false;
	}

	Type *expected_type = parent->ast_kind == AST_SWITCH_STMT ? cond->type : type_anyfault;

	if (!sema_analyse_expr_rhs(context, expected_type, value, false)) return false;

	statement->nextcase_stmt.defer_id = context_get_defers(context, context->active_scope.defer_last, parent->switch_stmt.defer, true);

	if (expr_is_const(value))
	{
		VECEACH(parent->switch_stmt.cases, i)
		{
			Ast *case_stmt = parent->switch_stmt.cases[i];
			Expr *from = exprptr(case_stmt->case_stmt.expr);
			if (case_stmt->ast_kind == AST_DEFAULT_STMT) continue;
			if (!expr_is_const(from)) goto VARIABLE_JUMP;
			ExprConst *const_expr = &from->const_expr;
			ExprConst *to_const_expr = case_stmt->case_stmt.to_expr ? &exprptr(case_stmt->case_stmt.to_expr)->const_expr : const_expr;
			if (expr_const_in_range(&value->const_expr, const_expr, to_const_expr))
			{
				statement->nextcase_stmt.case_switch_stmt = astid(case_stmt);
				return true;
			}
		}
		SEMA_ERROR(value, "There is no 'case %s' in the switch, please check if a case is missing or if this value is incorrect.", expr_const_to_error_string(&value->const_expr));
		return false;
	}
VARIABLE_JUMP:
	statement->nextcase_stmt.case_switch_stmt = astid(parent);
	statement->nextcase_stmt.switch_expr = value;
	return true;
}



static inline bool sema_analyse_then_overwrite(SemaContext *context, Ast *statement, AstId replacement)
{
	if (!replacement)
	{
		statement->ast_kind = AST_NOP_STMT;
		return true;
	}
	Ast *last = NULL;
	AstId next = statement->next;
	*statement = *astptr(replacement);
	AstId current = astid(statement);
	assert(current);
	while (current)
	{
		Ast *ast = ast_next(&current);
		if (!sema_analyse_statement(context, ast)) return false;
		last = ast;
	}
	last = ast_last(last);
	last->next = next;
	return true;
}


static inline bool sema_analyse_ct_if_stmt(SemaContext *context, Ast *statement)
{
	unsigned ct_context = sema_context_push_ct_stack(context);
	int res = sema_check_comp_time_bool(context, statement->ct_if_stmt.expr);
	if (res == -1) goto FAILED;
	if (res)
	{
		if (sema_analyse_then_overwrite(context, statement, statement->ct_if_stmt.then)) goto SUCCESS;
		goto FAILED;
	}
	Ast *elif = astptrzero(statement->ct_if_stmt.elif);
	while (1)
	{
		if (!elif)
		{
			// Turn into NOP!
			statement->ast_kind = AST_NOP_STMT;
			goto SUCCESS;
		}
		// We found else, then just replace with that.
		if (elif->ast_kind == AST_CT_ELSE_STMT)
		{
			if (sema_analyse_then_overwrite(context, statement, elif->ct_else_stmt)) goto SUCCESS;
			goto FAILED;
		}
		assert(elif->ast_kind == AST_CT_IF_STMT);

		res = sema_check_comp_time_bool(context, elif->ct_if_stmt.expr);
		if (res == -1) goto FAILED;
		if (res)
		{
			if (sema_analyse_then_overwrite(context, statement, elif->ct_if_stmt.then)) goto SUCCESS;
			goto FAILED;
		}
		elif = astptrzero(elif->ct_if_stmt.elif);
	}
SUCCESS:
	sema_context_pop_ct_stack(context, ct_context);
	return true;
FAILED:
	sema_context_pop_ct_stack(context, ct_context);
	return false;
}

static inline bool sema_analyse_compound_statement_no_scope(SemaContext *context, Ast *compound_statement)
{
	bool all_ok = ast_ok(compound_statement);
	AstId current = compound_statement->compound_stmt.first_stmt;
	Ast *ast = NULL;
	while (current)
	{
		ast = ast_next(&current);
		if (!sema_analyse_statement(context, ast))
		{
			ast_poison(ast);
			all_ok = false;
		}
	}
	AstId *next = ast ? &ast->next : &compound_statement->compound_stmt.first_stmt;
	context_pop_defers(context, next);
	return all_ok;
}

static inline bool sema_check_type_case(SemaContext *context, Type *switch_type, Ast *case_stmt, Ast **cases, unsigned index)
{
	Expr *expr = exprptr(case_stmt->case_stmt.expr);
	if (!sema_analyse_expr_rhs(context, type_typeid, expr, false)) return false;

	if (expr_is_const(expr))
	{
		Type *my_type = expr->const_expr.typeid;
		for (unsigned i = 0; i < index; i++)
		{
			Ast *other = cases[i];
			if (other->ast_kind != AST_CASE_STMT) continue;
			Expr *other_expr = exprptr(other->case_stmt.expr);
			if (expr_is_const(other_expr) && other_expr->const_expr.typeid == my_type)
			{
				SEMA_ERROR(case_stmt, "The same type appears more than once.");
				SEMA_NOTE(other, "Here is the case with that type.");
				return false;
			}
		}
	}
	return true;
}

static inline bool sema_check_value_case(SemaContext *context, Type *switch_type, Ast *case_stmt, Ast **cases, unsigned index, bool *if_chained, bool *max_ranged)
{
	assert(switch_type);
	Expr *expr = exprptr(case_stmt->case_stmt.expr);
	Expr *to_expr = exprptrzero(case_stmt->case_stmt.to_expr);

	// 1. Try to do implicit conversion to the correct type.
	if (!sema_analyse_expr_rhs(context, switch_type, expr, false)) return false;
	if (to_expr && !sema_analyse_expr_rhs(context, switch_type, to_expr, false)) return false;

	bool is_range = to_expr != NULL;
	bool first_is_const = expr_is_const(expr);
	if (!is_range && !first_is_const)
	{
		*if_chained = true;
		return true;
	}
	if (is_range && (!expr_is_const_int(expr) || !expr_is_const(to_expr)))
	{
		sema_error_at(extend_span_with_token(expr->span, to_expr->span), "Ranges must be constant integers.");
		return false;
	}
	ExprConst *const_expr = &expr->const_expr;
	ExprConst *to_const_expr = to_expr ? &to_expr->const_expr : const_expr;

	if (!*max_ranged && is_range)
	{
		if (int_comp(const_expr->ixx, to_const_expr->ixx, BINARYOP_GT))
		{
			sema_error_at(extend_span_with_token(expr->span, to_expr->span),
						  "The range is not valid because the first value (%s) is greater than the second (%s). "
						  "It would work if you swapped their order.",
						  int_to_str(const_expr->ixx, 10),
						  int_to_str(to_const_expr->ixx, 10));
			return false;
		}
		Int128 range = int_sub(to_const_expr->ixx, const_expr->ixx).i;
		Int128 max_range = { .low = active_target.switchrange_max_size };
		if (i128_comp(range, max_range, type_i128) == CMP_GT)
		{
			*max_ranged = true;
		}
	}
	for (unsigned i = 0; i < index; i++)
	{
		Ast *other = cases[i];
		if (other->ast_kind != AST_CASE_STMT) continue;
		Expr *other_expr = exprptr(other->case_stmt.expr);
		if (!expr_is_const(other_expr)) continue;
		ExprConst *other_const = &other_expr->const_expr;
		ExprConst *other_to_const = other->case_stmt.to_expr ? &exprptr(other->case_stmt.to_expr)->const_expr : other_const;
		if (expr_const_in_range(const_expr, other_const, other_to_const))
		{
			SEMA_ERROR(case_stmt, "The same case value appears more than once.");
			SEMA_NOTE(other, "Here is the previous use of that value.");
			return false;
		}
	}
	return true;
}

INLINE const char *create_missing_enums_in_switch_error(Ast **cases, unsigned case_count, Decl **enums)
{
	unsigned missing = vec_size(enums) - case_count;
	scratch_buffer_clear();
	if (missing == 1)
	{
		scratch_buffer_append("Enum value ");
	}
	else
	{
		scratch_buffer_printf("%u enum values were not handled in the switch: ", missing);
	}
	unsigned printed = 0;
	FOREACH_BEGIN(Decl *decl, enums)
		for (unsigned i = 0; i < case_count; i++)
		{
			Expr *e = exprptr(cases[i]->case_stmt.expr);
			assert(expr_is_const_enum(e));
			if (e->const_expr.enum_err_val == decl) goto CONTINUE;
		}
		if (++printed != 1)
		{
			scratch_buffer_append(printed == missing ? " and " : ", ");
		}
		scratch_buffer_append(decl->name);
		if (printed > 2 && missing > 3)
		{
			scratch_buffer_append(", ...");
			goto DONE;
		}
		if (printed == missing) goto DONE;
CONTINUE:;
	FOREACH_END();
DONE:;
	if (missing == 1)
	{
		scratch_buffer_append(" was not handled in the switch - either add it or add 'default'.");
		return scratch_buffer_to_string();
	}
	scratch_buffer_append(" - either add them or use 'default'.");
	return scratch_buffer_to_string();
}
static bool sema_analyse_switch_body(SemaContext *context, Ast *statement, SourceSpan expr_span, Type *switch_type, Ast **cases, ExprAnySwitch *any_switch, Decl *var_holder)
{
	bool use_type_id = false;
	if (!type_is_comparable(switch_type))
	{
		sema_error_at(expr_span, "You cannot test '%s' for equality, and only values that supports '==' for comparison can be used in a switch.", type_to_error_string(switch_type));
		return false;
	}
	// We need an if-chain if this isn't an enum/integer type.
	Type *flat = type_flatten(switch_type);
	TypeKind flat_switch_type_kind = flat->type_kind;
	bool is_enum_switch = flat_switch_type_kind == TYPE_ENUM;
	bool if_chain = !is_enum_switch && !type_kind_is_any_integer(flat_switch_type_kind);

	Ast *default_case = NULL;
	assert(context->active_scope.defer_start == context->active_scope.defer_last);

	bool exhaustive = false;
	unsigned case_count = vec_size(cases);
	bool success = true;
	bool max_ranged = false;
	bool type_switch = switch_type == type_typeid;
	for (unsigned i = 0; i < case_count; i++)
	{
		if (!success) break;
		Ast *stmt = cases[i];
		Ast *next = (i < case_count - 1) ? cases[i + 1] : NULL;
		PUSH_NEXT(next, statement);
		switch (stmt->ast_kind)
		{
			case AST_CASE_STMT:
				if (type_switch)
				{
					if (!sema_check_type_case(context, switch_type, stmt, cases, i))
					{
						success = false;
						break;;
					}
				}
				else
				{
					if (!sema_check_value_case(context, switch_type, stmt, cases, i, &if_chain, &max_ranged))
					{
						success = false;
						break;
					}
				}
				break;
			case AST_DEFAULT_STMT:
				exhaustive = true;
				if (default_case)
				{
					SEMA_ERROR(stmt, "'default' may only appear once in a single 'switch', please remove one.");
					SEMA_NOTE(default_case, "Here is the previous use.");
					success = false;
				}
				default_case = stmt;
				break;
			default:
				UNREACHABLE;
		}
		POP_NEXT();
	}

	if (!exhaustive && is_enum_switch) exhaustive = case_count >= vec_size(flat->decl->enums.values);
	bool all_jump_end = exhaustive;
	for (unsigned i = 0; i < case_count; i++)
	{
		Ast *stmt = cases[i];
		SCOPE_START
			PUSH_BREAK(statement);
			Ast *next = (i < case_count - 1) ? cases[i + 1] : NULL;
			PUSH_NEXT(next, statement);
			Ast *body = stmt->case_stmt.body;
			if (stmt->ast_kind == AST_CASE_STMT && body && type_switch && var_holder && expr_is_const(exprptr(stmt->case_stmt.expr)))
			{
				if (any_switch->is_assign)
				{
					Type *real_type = type_get_ptr(exprptr(stmt->case_stmt.expr)->const_expr.typeid);
					Decl *new_var = decl_new_var(any_switch->new_ident, any_switch->span,
												 type_info_new_base(any_switch->is_deref
												 ? real_type->pointer : real_type, any_switch->span),
												 VARDECL_LOCAL);
					Expr *var_result = expr_variable(var_holder);
					if (!cast_explicit(context, var_result, real_type)) return false;
					if (any_switch->is_deref)
					{
						expr_rewrite_insert_deref(var_result);
					}
					new_var->var.init_expr = var_result;
					Ast *decl_ast = new_ast(AST_DECLARE_STMT, new_var->span);
					decl_ast->declare_stmt = new_var;
					ast_prepend(&body->compound_stmt.first_stmt, decl_ast);
				}
				else
				{
					Expr *expr = exprptr(stmt->case_stmt.expr);
					Type *type = type_get_ptr(expr->const_expr.typeid);
					Decl *alias = decl_new_var(var_holder->name, var_holder->span,
											   type_info_new_base(type, expr->span),
											   VARDECL_LOCAL);
					Expr *ident_converted = expr_variable(var_holder);
					if (!cast_explicit(context, ident_converted, type)) return false;
					alias->var.init_expr = ident_converted;
					alias->var.shadow = true;
					Ast *decl_ast = new_ast(AST_DECLARE_STMT, alias->span);
					decl_ast->declare_stmt = alias;
					ast_prepend(&body->compound_stmt.first_stmt, decl_ast);
				}
			}
			success = success && (!body || sema_analyse_compound_statement_no_scope(context, body));
			POP_BREAK();
			POP_NEXT();
			if (!body && i < case_count - 1) continue;
			all_jump_end &= context->active_scope.jump_end;
		SCOPE_END;
	}
	if (is_enum_switch && !exhaustive && success)
	{
		SEMA_ERROR(statement, create_missing_enums_in_switch_error(cases, case_count, flat->decl->enums.values));
		success = false;
	}
	statement->flow.no_exit = all_jump_end;
	statement->switch_stmt.flow.if_chain = if_chain || max_ranged;
	return success;
}

static inline bool sema_analyse_ct_switch_stmt(SemaContext *context, Ast *statement)
{
	unsigned ct_context = sema_context_push_ct_stack(context);
	// Evaluate the switch statement
	Expr *cond = exprptrzero(statement->ct_switch_stmt.cond);
	if (cond && !sema_analyse_ct_expr(context, cond)) goto FAILED;

	// If we have a type, then we do different evaluation
	// compared to when it is a value.
	Type *type = cond ? cond->type : type_bool;
	bool is_type = false;
	switch (type_flatten(type)->type_kind)
	{
		case TYPE_TYPEID:
			is_type = true;
			FALLTHROUGH;
		case TYPE_ENUM:
		case ALL_INTS:
		case ALL_FLOATS:
		case TYPE_BOOL:
			break;
		case TYPE_SUBARRAY:
			if (expr_is_const_string(cond)) break;
			FALLTHROUGH;
		default:
			assert(cond);
			SEMA_ERROR(cond, "Only types, strings, enums, integers, floats and booleans may be used with '$switch'.");
			goto FAILED;
	}

	ExprConst *switch_expr_const = cond ? &cond->const_expr : NULL;
	Ast **cases = statement->ct_switch_stmt.body;

	unsigned case_count = vec_size(cases);
	assert(case_count <= INT32_MAX);
	int matched_case = (int)case_count;
	int default_case = (int)case_count;

	// Go through each case
	for (unsigned i = 0; i < case_count; i++)
	{
		Ast *stmt = cases[i];
		switch (stmt->ast_kind)
		{
			case AST_CASE_STMT:
			{
				Expr *expr = exprptr(stmt->case_stmt.expr);
				Expr *to_expr = exprptrzero(stmt->case_stmt.to_expr);
				if (to_expr && !type_is_integer(type))
				{
					SEMA_ERROR(to_expr, "$case ranges are only allowed for integers.");
					goto FAILED;
				}
				if (is_type)
				{
					if (!sema_analyse_ct_expr(context, expr)) goto FAILED;
					if (expr->type != type_typeid)
					{
						SEMA_ERROR(expr, "A type was expected here not %s.", type_quoted_error_string(expr->type));
						goto FAILED;
					}
				}
				else
				{
					if (!sema_analyse_expr_rhs(context, type, expr, false)) goto FAILED;
					if (to_expr && !sema_analyse_expr_rhs(context, type, to_expr, false)) goto FAILED;
				}
				if (!expr_is_const(expr))
				{
					SEMA_ERROR(expr, "The $case must have a constant expression.");
					goto FAILED;
				}
				if (!cond)
				{
					if (!expr->const_expr.b) continue;
					if (matched_case == case_count) matched_case = (int)i;
					continue;
				}
				if (to_expr && !expr_is_const(to_expr))
				{
					SEMA_ERROR(to_expr, "The $case must have a constant expression.");
					goto FAILED;
				}
				ExprConst *const_expr = &expr->const_expr;
				ExprConst *const_to_expr = to_expr ? &to_expr->const_expr : const_expr;
				if (to_expr && expr_const_compare(const_expr, const_to_expr, BINARYOP_GT))
				{
					SEMA_ERROR(to_expr, "The end of a range must be less or equal to the beginning.");
					goto FAILED;
				}
				// Check that it is unique.
				for (unsigned j = 0; j < i; j++)
				{
					Ast *other_stmt = cases[j];
					if (other_stmt->ast_kind == AST_DEFAULT_STMT) continue;
					ExprConst *other_const = &exprptr(other_stmt->case_stmt.expr)->const_expr;
					ExprConst *other_const_to = other_stmt->case_stmt.to_expr ? &exprptr(other_stmt->case_stmt.to_expr)->const_expr : other_const;
					if (expr_const_in_range(const_expr, other_const, other_const_to))
					{
						SEMA_ERROR(stmt, "'%s' appears more than once.", expr_const_to_error_string(const_expr));
						SEMA_NOTE(exprptr(cases[j]->case_stmt.expr), "The previous $case was here.");
						goto FAILED;
					}
				}
				if (is_type)
				{
					assert(const_expr == const_to_expr);
					Type *switch_type = switch_expr_const->typeid;
					Type *case_type = const_expr->typeid;
					if (matched_case > i && type_is_subtype(case_type->canonical, switch_type->canonical))
					{
						matched_case = (int)i;
					}
				}
				else if (expr_const_in_range(switch_expr_const, const_expr, const_to_expr))
				{
					matched_case = (int)i;
				}
				break;
			}
			case AST_DEFAULT_STMT:
				if (default_case < case_count)
				{
					SEMA_ERROR(stmt, "More than one $default is not allowed.");
					SEMA_NOTE(cases[default_case], "The previous $default was here.");
					goto FAILED;
				}
				default_case = (int)i;
				continue;
			default:
				UNREACHABLE;
		}
	}

	if (matched_case == case_count) matched_case = default_case;

	Ast *body = NULL;
	for (int i = matched_case; i < case_count; i++)
	{
		body = cases[i]->case_stmt.body;
		if (body) break;
	}
	if (!body)
	{
		statement->ast_kind = AST_NOP_STMT;
		goto SUCCESS;
	}
	if (!sema_analyse_then_overwrite(context, statement, body->compound_stmt.first_stmt)) goto FAILED;
SUCCESS:
	sema_context_pop_ct_stack(context, ct_context);
	return true;
FAILED:
	sema_context_pop_ct_stack(context, ct_context);
	return false;
}


static inline bool sema_analyse_ct_foreach_stmt(SemaContext *context, Ast *statement)
{
	unsigned ct_context = sema_context_push_ct_stack(context);
	Expr *collection = exprptr(statement->ct_foreach_stmt.expr);
	if (!sema_analyse_ct_expr(context, collection)) return false;
	if (!expr_is_const_untyped_list(collection) && !expr_is_const_initializer(collection))
	{
		SEMA_ERROR(collection, "Expected a list to iterate over");
		goto FAILED;
	}
	unsigned count;
	ConstInitializer *initializer = NULL;
	Expr **expressions = NULL;
	Type *const_list_type = NULL;
	if (expr_is_const_initializer(collection))
	{
		initializer = collection->const_expr.initializer;
		ConstInitType init_type = initializer->kind;
		const_list_type = type_flatten(collection->type);
		if (const_list_type->type_kind == TYPE_ARRAY || const_list_type->type_kind == TYPE_VECTOR)
		{
			count = const_list_type->array.len;
		}
		else
		{
			// Empty list
			if (init_type == CONST_INIT_ZERO)
			{
				sema_context_pop_ct_stack(context, ct_context);
				statement->ast_kind = AST_NOP_STMT;
				return true;
			}
			if (init_type != CONST_INIT_ARRAY_FULL)
			{
				SEMA_ERROR(collection, "Only regular arrays are allowed here.");
				goto FAILED;
			}
			count = vec_size(initializer->init_array_full);
		}
	}
	else
	{
		expressions = collection->const_expr.untyped_list;
		count = vec_size(expressions);
	}
	Decl *index = declptrzero(statement->ct_foreach_stmt.index);

	AstId start = 0;
	if (index)
	{
		index->type = type_int;
		if (!sema_add_local(context, index)) goto FAILED;
	}
	Decl *value = declptr(statement->ct_foreach_stmt.value);
	if (!sema_add_local(context, value)) goto FAILED;
	// Get the body
	Ast *body = astptr(statement->ct_foreach_stmt.body);
	AstId *current = &start;
	unsigned loop_context = sema_context_push_ct_stack(context);
	for (unsigned i = 0; i < count; i++)
	{
		sema_context_pop_ct_stack(context, loop_context);
		Ast *compound_stmt = copy_ast_single(body);
		if (expressions)
		{
			value->var.init_expr = expressions[i];
		}
		else
		{
			Expr *expr = expr_new(EXPR_CONST, collection->span);
			if (!expr_rewrite_to_const_initializer_index(const_list_type, initializer, expr, i, false))
			{
				SEMA_ERROR(collection, "Complex expressions are not allowed.");
				goto FAILED;
			}
			value->var.init_expr = expr;
		}
		if (index)
		{
			index->var.init_expr = expr_new_const_int(index->span, type_int, i);
			index->type = type_int;
		}
		if (!sema_analyse_compound_stmt(context, compound_stmt)) goto FAILED;
		*current = astid(compound_stmt);
		current = &compound_stmt->next;
	}
	sema_context_pop_ct_stack(context, ct_context);
	statement->ast_kind = AST_COMPOUND_STMT;
	statement->compound_stmt.first_stmt = start;
	return true;
FAILED:
	sema_context_pop_ct_stack(context, ct_context);
	return false;
}

static inline bool sema_analyse_switch_stmt(SemaContext *context, Ast *statement)
{
	statement->switch_stmt.scope_defer = context->active_scope.in_defer;

	SCOPE_START_WITH_LABEL(statement->switch_stmt.flow.label);

		Expr *cond = exprptrzero(statement->switch_stmt.cond);
		Type *switch_type;

		ExprAnySwitch var_switch;
		Decl *any_decl = NULL;
		if (statement->ast_kind == AST_SWITCH_STMT)
		{
			if (cond && !sema_analyse_cond(context, cond, COND_TYPE_EVALTYPE_VALUE)) return false;
			Expr *last = cond ? VECLAST(cond->cond_expr) : NULL;
			switch_type = last ? last->type->canonical : type_bool;
			if (last && last->expr_kind == EXPR_ANYSWITCH)
			{
				var_switch = last->any_switch;
				Expr *inner;
				if (var_switch.is_assign)
				{
					inner = expr_new(EXPR_DECL, last->span);
					any_decl = decl_new_generated_var(type_any, VARDECL_LOCAL, last->span);
					any_decl->var.init_expr = var_switch.any_expr;
					inner->decl_expr = any_decl;
					if (!sema_analyse_expr(context, inner)) return false;
				}
				else
				{
					inner = expr_new(EXPR_IDENTIFIER, last->span);
					any_decl = var_switch.variable;
					expr_resolve_ident(inner, any_decl);
					inner->type = type_any;
				}
				expr_rewrite_to_builtin_access(last, inner, ACCESS_TYPEOFANY, type_typeid);
				switch_type = type_typeid;
				cond->type = type_typeid;
			}

		}
		else
		{
			switch_type = type_anyfault;
		}

		statement->switch_stmt.defer = context->active_scope.defer_last;
		if (!sema_analyse_switch_body(context, statement, cond ? cond->span : statement->span,
									  switch_type->canonical,
									  statement->switch_stmt.cases, any_decl ? &var_switch : NULL, any_decl))
		{
			return SCOPE_POP_ERROR();
		}
		context_pop_defers_and_replace_ast(context, statement);
	SCOPE_END;

	if (statement->flow.no_exit && !statement->flow.has_break)
	{
		context->active_scope.jump_end = true;
	}
	return true;
}

bool sema_analyse_ct_assert_stmt(SemaContext *context, Ast *statement)
{
	Expr *expr = exprptrzero(statement->assert_stmt.expr);
	ExprId message = statement->assert_stmt.message;
	const char *msg = NULL;
	Expr *message_expr = message ? exprptr(message) : NULL;
	if (message_expr)
	{
		if (!sema_analyse_expr(context, message_expr)) return false;
		if (message_expr->expr_kind != EXPR_CONST || message_expr->const_expr.const_kind != CONST_STRING)
		{
			SEMA_ERROR(message_expr, "Expected a string as the error message.");
		}
	}
	int res = expr ? sema_check_comp_time_bool(context, expr) : 0;

	if (res == -1) return false;
	SourceSpan span = expr ? expr->span : statement->span;
	if (!res)
	{
		if (context->current_macro)
		{
			if (message_expr)
			{
				sema_error_at(context->inlining_span, "%.*s", EXPAND_EXPR_STRING(message_expr));
			}
			else
			{
				sema_error_at(context->inlining_span, "Compile time assert failed.");
			}
			sema_error_prev_at(span, expr ? "$assert was defined here." : "$error was defined here");
			return false;
		}
		if (message_expr)
		{
			sema_error_at(span, "%.*s", EXPAND_EXPR_STRING(message_expr));
		}
		else
		{
			sema_error_at(span, "Compile time assert failed.");
		}
		return false;
	}
	statement->ast_kind = AST_NOP_STMT;
	return true;
}

bool sema_analyse_ct_echo_stmt(SemaContext *context, Ast *statement)
{
	Expr *message = statement->expr_stmt;
	if (!sema_analyse_expr(context, message)) return false;
	if (message->expr_kind != EXPR_CONST)
	{
		SEMA_ERROR(message, "Expected a constant value.");
		return false;
	}
	printf("] ");
	switch (message->const_expr.const_kind)
	{
		case CONST_FLOAT:
			printf("%f\n", (double)message->const_expr.fxx.f);
			break;
		case CONST_INTEGER:
			puts(int_to_str(message->const_expr.ixx, 10));
			break;
		case CONST_BOOL:
			puts(message->const_expr.b ? "true" : "false");
			break;
		case CONST_ENUM:
		case CONST_ERR:
			puts(message->const_expr.enum_err_val->name);
			break;
		case CONST_STRING:
			printf("%.*s\n", EXPAND_EXPR_STRING(message));
			break;
		case CONST_POINTER:
			printf("%p\n", (void*)message->const_expr.ptr);
			break;
		case CONST_TYPEID:
			puts(type_to_error_string(message->const_expr.typeid));
			break;
		case CONST_BYTES:
		case CONST_INITIALIZER:
		case CONST_UNTYPED_LIST:
		case CONST_MEMBER:
			SEMA_ERROR(message, "Unsupported type for '$echo'");
			break;
	}
	statement->ast_kind = AST_NOP_STMT;
	return true;
}

/**
 * $for(<list of ct decl/expr>, <cond>, <incr>):
 */
static inline bool sema_analyse_ct_for_stmt(SemaContext *context, Ast *statement)
{
	unsigned for_context = sema_context_push_ct_stack(context);
	bool success = false;
	ExprId init;
	if ((init = statement->for_stmt.init))
	{
		Expr *init_expr = exprptr(init);
		assert(init_expr->expr_kind == EXPR_EXPRESSION_LIST);

		// Check the list of expressions.
		FOREACH_BEGIN(Expr *expr, init_expr->expression_list)
			// Only a subset of declarations are allowed. We check this here.
			if (expr->expr_kind == EXPR_DECL)
			{
				Decl *decl = expr->decl_expr;
				if (decl->decl_kind != DECL_VAR || (decl->var.kind != VARDECL_LOCAL_CT && decl->var.kind != VARDECL_LOCAL_CT_TYPE))
				{
					SEMA_ERROR(expr, "Only 'var $foo' and 'var $Type' declarations are allowed in '$for'");
					goto FAILED;
				}
				if (!sema_analyse_var_decl_ct(context, decl)) goto FAILED;
				continue;
			}
			// If expression evaluate it and make sure it is constant.
			if (!sema_analyse_ct_expr(context, expr)) goto FAILED;
		FOREACH_END();
	}
	ExprId condition = statement->for_stmt.cond;
	ExprId incr = statement->for_stmt.incr;
	Ast *body = astptr(statement->for_stmt.body);
	AstId start = 0;
	AstId *current = &start;
	Expr **incr_list = incr ? exprptr(incr)->expression_list : NULL;
	assert(condition);
	// We set a maximum of macro iterations.
	// we might consider reducing this.
	unsigned current_ct_scope = sema_context_push_ct_stack(context);
	for (int i = 0; i < MAX_MACRO_ITERATIONS; i++)
	{
		sema_context_pop_ct_stack(context, current_ct_scope);
		// First evaluate the cond, which we note that we *must* have.
		// we need to make a copy
		Expr *copy = copy_expr_single(exprptr(condition));
		if (!sema_analyse_cond_expr(context, copy)) goto FAILED;
		if (!expr_is_const(copy))
		{
			SEMA_ERROR(copy, "Expected a value that can be evaluated at compile time.");
			goto FAILED;
		}
		// This is simple, since we know we have a boolean, just break if we reached "false"
		if (!copy->const_expr.b) break;

		// Otherwise we copy the body.
		Ast *compound_stmt = copy_ast_single(body);

		// Analyse the body
		if (!sema_analyse_compound_statement_no_scope(context, compound_stmt)) goto FAILED;

		// Append it.
		*current = astid(compound_stmt);
		current = &compound_stmt->next;

		// Copy and evaluate all the expressions in "incr"
		FOREACH_BEGIN(Expr *expr, incr_list)
			if (!sema_analyse_ct_expr(context, copy_expr_single(expr))) goto FAILED;
		FOREACH_END();
	}
	// Analysis is done turn the generated statements into a compound statement for lowering.
	statement->ast_kind = AST_COMPOUND_STMT;
	statement->compound_stmt = (AstCompoundStmt) { .first_stmt = start };
	return true;
FAILED:
	sema_context_pop_ct_stack(context, for_context);
	return false;
}


static inline bool sema_analyse_statement_inner(SemaContext *context, Ast *statement)
{
	switch (statement->ast_kind)
	{
		case AST_POISONED:
		case AST_IF_CATCH_SWITCH_STMT:
		case AST_CONTRACT:
		case AST_ASM_STMT:
		case AST_CONTRACT_FAULT:
			UNREACHABLE
		case AST_DECLS_STMT:
			return sema_analyse_decls_stmt(context, statement);
		case AST_ASM_BLOCK_STMT:
			return sema_analyse_asm_stmt(context, statement);
		case AST_ASSERT_STMT:
			return sema_analyse_assert_stmt(context, statement);
		case AST_BREAK_STMT:
			return sema_analyse_break_stmt(context, statement);
		case AST_CASE_STMT:
			SEMA_ERROR(statement, "Unexpected 'case' outside of switch");
			return false;
		case AST_COMPOUND_STMT:
			return sema_analyse_compound_stmt(context, statement);
		case AST_CONTINUE_STMT:
			return sema_analyse_continue_stmt(context, statement);
		case AST_CT_ASSERT:
			return sema_analyse_ct_assert_stmt(context, statement);
		case AST_CT_IF_STMT:
			return sema_analyse_ct_if_stmt(context, statement);
		case AST_CT_ECHO_STMT:
			return sema_analyse_ct_echo_stmt(context, statement);
		case AST_DECLARE_STMT:
			return sema_analyse_declare_stmt(context, statement);
		case AST_DEFAULT_STMT:
			SEMA_ERROR(statement, "Unexpected 'default' outside of switch");
			return false;
		case AST_DEFER_STMT:
			return sema_analyse_defer_stmt(context, statement);
		case AST_EXPR_STMT:
			return sema_analyse_expr_stmt(context, statement);
		case AST_FOREACH_STMT:
			return sema_analyse_foreach_stmt(context, statement);
		case AST_FOR_STMT:
			return sema_analyse_for_stmt(context, statement);
		case AST_IF_STMT:
			return sema_analyse_if_stmt(context, statement);
		case AST_NOP_STMT:
			return true;
		case AST_BLOCK_EXIT_STMT:
			UNREACHABLE
		case AST_RETURN_STMT:
			return sema_analyse_return_stmt(context, statement);
		case AST_SWITCH_STMT:
			return sema_analyse_switch_stmt(context, statement);
		case AST_NEXTCASE_STMT:
			return sema_analyse_nextcase_stmt(context, statement);
		case AST_CT_SWITCH_STMT:
			return sema_analyse_ct_switch_stmt(context, statement);
		case AST_CT_ELSE_STMT:
			UNREACHABLE
		case AST_CT_FOREACH_STMT:
			return sema_analyse_ct_foreach_stmt(context, statement);
		case AST_CT_FOR_STMT:
			return sema_analyse_ct_for_stmt(context, statement);
	}

	UNREACHABLE
}


bool sema_analyse_statement(SemaContext *context, Ast *statement)
{
	if (statement->ast_kind == AST_POISONED) return false;
	bool dead_code = context->active_scope.jump_end;
	if (!sema_analyse_statement_inner(context, statement)) return ast_poison(statement);
	if (dead_code)
	{
		if (!context->active_scope.allow_dead_code)
		{
			context->active_scope.allow_dead_code = true;
			// If we start with an don't start with an assert AND the scope is a macro, then it's bad.
			if (statement->ast_kind != AST_ASSERT_STMT && statement->ast_kind != AST_NOP_STMT && !(context->active_scope.flags & SCOPE_MACRO))
			{
				SEMA_ERROR(statement, "This code will never execute.");
				return ast_poison(statement);
			}
			// Remove it
			statement->ast_kind = AST_NOP_STMT;
		}
	}
	return true;
}


static bool sema_analyse_require(SemaContext *context, Ast *directive, AstId **asserts, SourceSpan span)
{
	return assert_create_from_contract(context, directive, asserts, span);
}

static bool sema_analyse_ensure(SemaContext *context, Ast *directive)
{
	Expr *declexpr = directive->contract_stmt.contract.decl_exprs;
	assert(declexpr->expr_kind == EXPR_EXPRESSION_LIST);

	VECEACH(declexpr->expression_list, j)
	{
		Expr *expr = declexpr->expression_list[j];
		if (expr->expr_kind == EXPR_DECL)
		{
			SEMA_ERROR(expr, "Only expressions are allowed.");
			return false;
		}
	}
	return true;
}

static bool sema_analyse_optional_returns(SemaContext *context, Ast *directive)
{
	Ast **returns = NULL;
	context->call_env.opt_returns = NULL;
	FOREACH_BEGIN(Ast *ret, directive->contract_stmt.faults)
		if (ret->contract_fault.resolved) continue;
		TypeInfo *type_info = ret->contract_fault.type;
		const char *ident = ret->contract_fault.ident;
		if (type_info->kind != TYPE_INFO_IDENTIFIER) RETURN_SEMA_ERROR(type_info, "Expected a fault name here.");
		if (!sema_resolve_type_info(context, type_info)) return false;
		Type *type = type_info->type;
		if (type->type_kind != TYPE_FAULTTYPE) RETURN_SEMA_ERROR(type_info, "A fault type is required.");
		if (!ident)
		{
			ret->contract_fault.decl = type->decl;
			ret->contract_fault.resolved = true;
			goto NEXT;
		}
		Decl *decl = type->decl;
		Decl **enums = decl->enums.values;
		VECEACH(enums, j)
		{
			Decl *opt_value = enums[j];
			if (opt_value->name == ident)
			{
				ret->contract_fault.decl = opt_value;
				ret->contract_fault.resolved = true;
				goto NEXT;
			}
		}
		RETURN_SEMA_ERROR(ret, "No fault value '%s' found.", ident);
NEXT:;
		vec_add(context->call_env.opt_returns, ret->contract_fault.decl);
	FOREACH_END();
	return true;
}

bool sema_analyse_checked(SemaContext *context, Ast *directive, SourceSpan span)
{
	Expr *declexpr = directive->contract_stmt.contract.decl_exprs;
	bool success = true;
	bool suppress_error = global_context.suppress_errors;
	global_context.suppress_errors = true;
	CallEnvKind eval_kind = context->call_env.kind;
	context->call_env.kind = CALL_ENV_CHECKS;
	SCOPE_START_WITH_FLAGS(SCOPE_CHECKS)
		VECEACH(declexpr->cond_expr, j)
		{
			Expr *expr = declexpr->cond_expr[j];
			if (sema_analyse_expr(context, expr)) continue;
			const char *comment = directive->contract_stmt.contract.comment;
			global_context.suppress_errors = suppress_error;
			sema_error_at(span.row == 0 ? expr->span : span, "Contraint failed: %s",
						  comment ? comment : directive->contract_stmt.contract.expr_string);
			success = false;
			goto END;
		}
END:
	context->call_env.kind = eval_kind;
	SCOPE_END;
	global_context.suppress_errors = suppress_error;
	return success;
}

void sema_append_contract_asserts(AstId assert_first, Ast* compound_stmt)
{
	assert(compound_stmt->ast_kind == AST_COMPOUND_STMT);
	if (!assert_first) return;
	Ast *ast = new_ast(AST_COMPOUND_STMT, compound_stmt->span);
	ast->compound_stmt.first_stmt = assert_first;
	ast_prepend(&compound_stmt->compound_stmt.first_stmt, ast);
}

bool sema_analyse_contracts(SemaContext *context, AstId doc, AstId **asserts, SourceSpan call_span, bool *has_ensures)
{
	while (doc)
	{
		Ast *directive = astptr(doc);
		switch (directive->contract_stmt.kind)
		{
			case CONTRACT_UNKNOWN:
			case CONTRACT_PURE:
				break;
			case CONTRACT_REQUIRE:
				if (!sema_analyse_require(context, directive, asserts, call_span)) return false;
				break;
			case CONTRACT_CHECKED:
				if (!sema_analyse_checked(context, directive, call_span)) return false;
				break;
			case CONTRACT_PARAM:
				break;
			case CONTRACT_OPTIONALS:
				if (!sema_analyse_optional_returns(context, directive)) return false;
				break;
			case CONTRACT_ENSURE:
				if (!sema_analyse_ensure(context, directive)) return false;
				*has_ensures = true;
				break;
		}
		doc = directive->next;
	}
	return true;
}

bool sema_analyse_function_body(SemaContext *context, Decl *func)
{
	if (!decl_ok(func)) return false;
	if (func->func_decl.attr_dynamic)
	{
		Decl *ambiguous = NULL;
		Decl *private = NULL;
		Decl *any = sema_resolve_type_method(context->unit, type_any, func->name, &ambiguous, &private);
		if (!any)
		{
			SEMA_ERROR(func, "To define a '@dynamic' method, the prototype method 'any.%s(...)' must exist. Did you spell the method name right?",
					   func->name);
			return false;
		}

		Signature any_sig = any->func_decl.signature;
		Signature this_sig = func->func_decl.signature;
		Type *any_rtype = typeget(any_sig.rtype);
		Type *this_rtype = typeget(this_sig.rtype);
		if (any_rtype->canonical != this_rtype->canonical)
		{
			SEMA_ERROR(type_infoptr(this_sig.rtype), "The prototype method has a return type %s, but this function returns %s, they need to match.",
					   type_quoted_error_string(any_rtype), type_quoted_error_string(this_rtype));
			SEMA_NOTE(type_infoptr(any_sig.rtype), "The interface definition is here.");
			return false;
		}
		Decl **any_params = any_sig.params;
		Decl **this_params = this_sig.params;
		unsigned any_param_count = vec_size(any_params);
		unsigned this_param_count = vec_size(this_params);
		if (any_param_count != this_param_count)
		{
			if (any_param_count > this_param_count)
			{
				SEMA_ERROR(func, "This function is missing parameters, %d parameters were expected.", any_param_count);
				SEMA_NOTE(any_params[this_param_count], "Compare with the interface definition.");
				return false;
			}
			else
			{
				SEMA_ERROR(this_params[any_param_count], "This function has too many parameters (%d).", this_param_count);
				SEMA_NOTE(any, "Compare with the interface, which has only %d parameter%s.",
						  any_param_count, any_param_count == 1 ? "" : "s");
			}
			return false;
		}
		FOREACH_BEGIN_IDX(i, Decl *param, this_params)
			if (i == 0) continue;
			if (param->type->canonical != any_params[i]->type->canonical)
			{
				SEMA_ERROR(vartype(param), "The prototype argument has type %s, but in this function it has type %s. Please make them match.",
						   type_quoted_error_string(any_params[i]->type), type_quoted_error_string(param->type));
				SEMA_NOTE(vartype(any_params[i]), "The interface definition is here.");
				return false;
			}

		FOREACH_END();
		func->func_decl.any_prototype = declid(any);
	}
	Signature *signature = &func->func_decl.signature;
	FunctionPrototype *prototype = func->type->function.prototype;
	assert(prototype);
	context->call_env = (CallEnv) {
		.current_function = func,
		.kind = CALL_ENV_FUNCTION,
		.pure = func->func_decl.signature.attrs.is_pure
	};
	context->rtype = prototype->rtype;
	context->macro_call_depth = 0;
	context->active_scope = (DynamicScope) {
			.scope_id = 0,
			.depth = 0,
			.label_start = 0,
			.current_local = 0
	};
	vec_resize(context->ct_locals, 0);

	// Clear returns
	vec_resize(context->returns, 0);
	context->scope_id = 0;
	context->continue_target = NULL;
	context->next_target = 0;
	context->next_switch = 0;
	context->break_target = 0;
	assert(func->func_decl.body);
	Ast *body = astptr(func->func_decl.body);
	Decl **lambda_params = NULL;
	SCOPE_START
		assert(context->active_scope.depth == 1);
		Decl **params = signature->params;
		VECEACH(params, i)
		{
			if (!sema_add_local(context, params[i])) return false;
		}
		if (func->func_decl.is_lambda)
		{
			lambda_params = copy_decl_list_single(func->func_decl.lambda_ct_parameters);
			FOREACH_BEGIN(Decl *ct_param, lambda_params)
				ct_param->var.is_read = false;
				if (!sema_add_local(context, ct_param)) return false;
			FOREACH_END();
		}
		AstId assert_first = 0;
		AstId *next = &assert_first;
		bool has_ensures = false;
		if (!sema_analyse_contracts(context, func->func_decl.docs, &next, INVALID_SPAN, &has_ensures)) return false;
		context->call_env.ensures = has_ensures;
		if (func->func_decl.attr_naked)
		{
			AstId current = body->compound_stmt.first_stmt;
			while (current)
			{
				Ast *stmt = ast_next(&current);
				if (stmt->ast_kind != AST_ASM_STMT)
				{
					SEMA_ERROR(stmt, "Only asm statements are allowed inside of a naked function.");
					return false;
				}
			}
			assert_first = 0;
		}
		else
		{
			sema_append_contract_asserts(assert_first, body);
			Type *canonical_rtype = type_no_optional(prototype->rtype)->canonical;
			if (!sema_analyse_compound_statement_no_scope(context, body)) return false;
			assert(context->active_scope.depth == 1);
			if (!context->active_scope.jump_end && canonical_rtype != type_void)
			{
				SEMA_ERROR(func, "Missing return statement at the end of the function.");
				return false;
			}
		}
	SCOPE_END;
	if (lambda_params)
	{
		FOREACH_BEGIN_IDX(i, Decl *ct_param, lambda_params)
			func->func_decl.lambda_ct_parameters[i]->var.is_read = ct_param->var.is_read;
		FOREACH_END();
	}
	return true;
}

