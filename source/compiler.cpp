#include "compiler.hpp"
#include "errors.hpp"
#include "compiler_context.hpp"
#include "expression.hpp"
#include "incomplete_function.hpp"
#include "tokenizer.hpp"
#include "runtime_context.hpp"
#include "helpers.hpp"

namespace stork {
	namespace {
		struct possible_flow {
			size_t break_level;
			bool can_continue;
			type_handle return_type_id;
			
			possible_flow add_switch() {
				return possible_flow{break_level+1, can_continue, return_type_id};
			}
			
			possible_flow add_loop() {
				return possible_flow{break_level+1, true, return_type_id};
			}
			
			static possible_flow in_function(type_handle return_type_id) {
				return possible_flow{0, false, return_type_id};
			}
		};
	
		bool is_typename(const compiler_context&, const tokens_iterator& it) {
			return std::visit(overloaded{
				[](reserved_token t) {
					switch (t) {
						case reserved_token::kw_number:
						case reserved_token::kw_string:
							return true;
						default:
							return false;
					}
				},
				[](const token_value&) {
					return false;
				}
			}, it->get_value());
		}
	
		error unexpected_syntax(const tokens_iterator& it) {
			return unexpected_syntax_error(std::to_string(it->get_value()), it->get_line_number(), it->get_char_index());
		}
		
		std::vector<expression<lvalue>::ptr> compile_variable_declaration(compiler_context& ctx, tokens_iterator& it) {
			type_handle type_id = parse_type(ctx, it);
			
			std::vector<expression<lvalue>::ptr> ret;
			
			do {
				if (!ret.empty()) {
					++it;
				}
			
				std::string name = parse_declaration_name(ctx, it);
			
				if (it->has_value(reserved_token::open_round)) {
					++it;
					ret.emplace_back(build_initialization_expression(ctx, it, type_id, false));
					parse_token_value(ctx, it, reserved_token::close_round);
				} else if (it->has_value(reserved_token::assign)) {
					++it;
					ret.emplace_back(build_initialization_expression(ctx, it, type_id, false));
				} else {
					ret.emplace_back(build_default_initialization(type_id));
				}
				
				ctx.create_identifier(std::move(name), type_id);
			} while (it->has_value(reserved_token::comma));
			
			return ret;
		}
		
		statement_ptr compile_simple_statement(compiler_context& ctx, tokens_iterator& it);
		
		block_statement_ptr compile_block_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_for_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_while_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_do_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_if_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_switch_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_var_statement(compiler_context& ctx, tokens_iterator& it);
		
		statement_ptr compile_break_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_continue_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_return_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf);
		
		statement_ptr compile_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf, bool in_switch) {
			if (it->is_reserved_token()) {
				switch (it->get_reserved_token()) {
					case reserved_token::kw_for:
						return compile_for_statement(ctx, it, pf.add_loop());
					case reserved_token::kw_while:
						return compile_while_statement(ctx, it, pf.add_loop());
					case reserved_token::kw_do:
						return compile_do_statement(ctx, it, pf.add_loop());
					case reserved_token::kw_if:
						return compile_if_statement(ctx, it, pf);
					case reserved_token::kw_switch:
						return compile_switch_statement(ctx, it, pf.add_switch());
					case reserved_token::kw_break:
						return compile_break_statement(ctx, it, pf);
					case reserved_token::kw_continue:
						return compile_continue_statement(ctx, it, pf);
					case reserved_token::kw_return:
						return compile_return_statement(ctx, it, pf);
					default:
						break;
				}
			}
			
			if (is_typename(ctx, it)) {
				if (in_switch) {
					throw syntax_error("Declarations in switch block are not allowed", it->get_line_number(), it->get_char_index());
				} else {
					return compile_var_statement(ctx, it);
				}
			}
			
			if (it->has_value(reserved_token::open_curly)) {
				return compile_block_statement(ctx, it, pf);
			}
			
			return compile_simple_statement(ctx, it);
		}
		
		statement_ptr compile_simple_statement(compiler_context& ctx, tokens_iterator& it) {
			statement_ptr ret = create_simple_statement(build_void_expression(ctx, it));
			parse_token_value(ctx, it, reserved_token::semicolon);
			return ret;
		}
		
		statement_ptr compile_for_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			parse_token_value(ctx, it, reserved_token::kw_for);
			parse_token_value(ctx, it, reserved_token::open_round);
			
			std::vector<expression<lvalue>::ptr> decls;
			expression<void>::ptr expr1;
			
			if (is_typename(ctx, it)) {
				decls = compile_variable_declaration(ctx, it);
			} else {
				expr1 = build_void_expression(ctx, it);
			}
		
			parse_token_value(ctx, it, reserved_token::semicolon);
			
			expression<number>::ptr expr2 = build_number_expression(ctx, it);
			parse_token_value(ctx, it, reserved_token::semicolon);
			
			expression<void>::ptr expr3 = build_void_expression(ctx, it);
			parse_token_value(ctx, it, reserved_token::close_round);
			
			block_statement_ptr block = compile_block_statement(ctx, it, pf);
			
			if (!decls.empty()) {
				return create_for_statement(std::move(decls), std::move(expr2), std::move(expr3), std::move(block));
			} else {
				return create_for_statement(std::move(expr1), std::move(expr2), std::move(expr3), std::move(block));
			}
		}
		
		statement_ptr compile_while_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			parse_token_value(ctx, it, reserved_token::kw_while);

			parse_token_value(ctx, it, reserved_token::open_round);
			expression<number>::ptr expr = build_number_expression(ctx, it);
			parse_token_value(ctx, it, reserved_token::close_round);
			
			block_statement_ptr block = compile_block_statement(ctx, it, pf);
			
			return create_while_statement(std::move(expr), std::move(block));
		}
		
		statement_ptr compile_do_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			parse_token_value(ctx, it, reserved_token::kw_do);
			
			block_statement_ptr block = compile_block_statement(ctx, it, pf);
			
			parse_token_value(ctx, it, reserved_token::kw_while);
			
			parse_token_value(ctx, it, reserved_token::open_round);
			expression<number>::ptr expr = build_number_expression(ctx, it);
			parse_token_value(ctx, it, reserved_token::close_round);
			
			return create_do_statement(std::move(expr), std::move(block));
		}
		
		statement_ptr compile_if_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			parse_token_value(ctx, it, reserved_token::kw_if);
			
			parse_token_value(ctx, it, reserved_token::open_round);
			
			std::vector<expression<lvalue>::ptr> decls;
			
			if (is_typename(ctx, it)) {
				decls = compile_variable_declaration(ctx, it);
				parse_token_value(ctx, it, reserved_token::semicolon);
			}
			
			std::vector<expression<number>::ptr> exprs;
			std::vector<block_statement_ptr> stmts;
			
			exprs.emplace_back(build_number_expression(ctx, it));
			parse_token_value(ctx, it, reserved_token::close_round);
			stmts.emplace_back(compile_block_statement(ctx, it, pf));
			
			while (it->has_value(reserved_token::kw_elif)) {
				++it;
				parse_token_value(ctx, it, reserved_token::open_round);
				exprs.emplace_back(build_number_expression(ctx, it));
				parse_token_value(ctx, it, reserved_token::close_round);
				stmts.emplace_back(compile_block_statement(ctx, it, pf));
			}
			
			if (it->has_value(reserved_token::kw_else)) {
				++it;
				stmts.emplace_back(compile_block_statement(ctx, it, pf));
			} else {
				stmts.emplace_back(create_block_statement({}));
			}
			
			return create_if_statement(std::move(decls), std::move(exprs), std::move(stmts));
		}
		
		 statement_ptr compile_switch_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			parse_token_value(ctx, it, reserved_token::kw_switch);
			
			parse_token_value(ctx, it, reserved_token::open_round);
			
			std::vector<expression<lvalue>::ptr> decls;
			
			if (is_typename(ctx, it)) {
				decls = compile_variable_declaration(ctx, it);
				parse_token_value(ctx, it, reserved_token::semicolon);
			}
			
			expression<number>::ptr expr = build_number_expression(ctx, it);
			parse_token_value(ctx, it, reserved_token::close_round);
			
			std::vector<statement_ptr> stmts;
			std::unordered_map<number, size_t> cases;
			size_t dflt = size_t(-1);
			
			parse_token_value(ctx, it, reserved_token::open_curly);
			
			while (!it->has_value(reserved_token::close_curly)) {
				if (it->has_value(reserved_token::kw_case)) {
					++it;
					if (!it->is_number()) {
						unexpected_syntax(it);
					}
					cases.emplace(it->get_number(), stmts.size());
					++it;
					parse_token_value(ctx, it, reserved_token::colon);
				} else if (it->has_value(reserved_token::kw_default)) {
					++it;
					dflt = stmts.size();
					parse_token_value(ctx, it, reserved_token::colon);
				} else {
					stmts.emplace_back(compile_statement(ctx, it, pf, true));
				}
			}
			
			++it;
			
			if (dflt == size_t(-1)) {
				dflt = stmts.size();
			}
			
			return create_switch_statement(std::move(decls), std::move(expr), std::move(stmts), std::move(cases), dflt);
		}
	
		statement_ptr  compile_var_statement(compiler_context& ctx, tokens_iterator& it) {
			std::vector<expression<lvalue>::ptr> decls = compile_variable_declaration(ctx, it);
			parse_token_value(ctx, it, reserved_token::semicolon);
			return create_local_declaration_statement(std::move(decls));
		}
		
		statement_ptr compile_break_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			if (pf.break_level == 0) {
				unexpected_syntax(it);
			}
			
			parse_token_value(ctx, it, reserved_token::kw_break);
			
			if (!it->is_number()) {
				unexpected_syntax(it);
			}
			
			double n = it->get_number();
			
			if (n < 1 || n != int(n)) {
				throw syntax_error("Invalid break value", it->get_line_number(), it->get_char_index());
			}
			
			++it;
			parse_token_value(ctx, it, reserved_token::semicolon);
			
			return create_break_statement(int(n));
		}
		
		statement_ptr compile_continue_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf){
			if (!pf.can_continue) {
				unexpected_syntax(it);
			}
			parse_token_value(ctx, it, reserved_token::kw_continue);
			parse_token_value(ctx, it, reserved_token::semicolon);
			return create_continue_statement();
		}
		
		statement_ptr compile_return_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf){
			parse_token_value(ctx, it, reserved_token::kw_return);
			
			if (pf.return_type_id == type_registry::get_void_handle()) {
				parse_token_value(ctx, it, reserved_token::semicolon);
				return create_return_void_statement();
			} else {
				expression<lvalue>::ptr expr = build_initialization_expression(ctx, it, pf.return_type_id, true);
				parse_token_value(ctx, it, reserved_token::semicolon);
				return create_return_statement(std::move(expr));
			}
		}
		
		
		std::vector<statement_ptr> compile_block_contents(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			std::vector<statement_ptr> ret;
			
			if (it->has_value(reserved_token::open_curly)) {
				parse_token_value(ctx, it, reserved_token::open_curly);
				
				while (!it->has_value(reserved_token::close_curly)) {
					ret.push_back(compile_statement(ctx, it, pf, false));
				}
				
				parse_token_value(ctx, it, reserved_token::close_curly);
			} else {
				ret.push_back(compile_statement(ctx, it, pf, false));
			}
			
			return ret;
		}
		
		block_statement_ptr compile_block_statement(compiler_context& ctx, tokens_iterator& it, possible_flow pf) {
			std::vector<statement_ptr> block = compile_block_contents(ctx, it, pf);
			return create_block_statement(std::move(block));
		}
	}

	void parse_token_value(compiler_context&, tokens_iterator& it, const token_value& value) {
		if (it->has_value(value)) {
			++it;
			return;
		}
		throw expected_syntax_error(std::to_string(value), it->get_line_number(), it->get_char_index());
	}
	
	std::string parse_declaration_name(compiler_context& ctx, tokens_iterator& it) {
		if (!it->is_identifier()) {
			throw unexpected_syntax(it);
		}

		std::string ret = it->get_identifier().name;
		
		if (!ctx.can_declare(ret)) {
			throw already_declared_error(ret, it->get_line_number(), it->get_char_index());
		}

		++it;
		
		return ret;
	}

	type_handle parse_type(compiler_context& ctx, tokens_iterator& it) {
		if (!it->is_reserved_token()) {
			throw unexpected_syntax(it);
		}
		
		type_handle t = nullptr;
		
		switch (it->get_reserved_token()) {
			case reserved_token::kw_void:
				t = ctx.get_handle(simple_type::nothing);
				break;
			case reserved_token::kw_number:
				t = ctx.get_handle(simple_type::number);
				break;
			case reserved_token::kw_string:
				t = ctx.get_handle(simple_type::string);
				break;
			default:
				throw unexpected_syntax(it);
		}
		
		++it;
		while (it->is_reserved_token()) {
			switch (it->get_reserved_token()) {
				case reserved_token::open_square:
					parse_token_value(ctx, ++it, reserved_token::close_square);
					t = ctx.get_handle(array_type{t});
					break;
				case reserved_token::open_round:
					{
						function_type ft;
						ft.return_type_id = t;
						++it;
						while (!it->has_value(reserved_token::close_round)) {
							if (!ft.param_type_id.empty()) {
								parse_token_value(ctx, it, reserved_token::comma);
							}
							type_handle param_type = parse_type(ctx, it);
							if (it->has_value(reserved_token::bitwise_and)) {
								ft.param_type_id.push_back({param_type, true});
								++it;
							} else {
								ft.param_type_id.push_back({param_type, false});
							}
						}
						++it;
						t = ctx.get_handle(ft);
					}
					break;
				default:
					break;
			}
		}
		
		return t;
	}
	
	shared_block_statement_ptr compile_function_block(compiler_context& ctx, tokens_iterator& it, type_handle return_type_id) {
		std::vector<statement_ptr> block = compile_block_contents(ctx, it, possible_flow::in_function(return_type_id));
		block.emplace_back(create_return_statement(build_default_initialization(return_type_id)));
		return create_shared_block_statement(std::move(block));
	}
	
	runtime_context compile(compiler_context& ctx, tokens_iterator& it) {
		if (!std::holds_alternative<reserved_token>(it->get_value())) {
			throw unexpected_syntax(it);
		}
		
		std::vector<expression<lvalue>::ptr> initializers;
		
		std::vector<incomplete_function> incomplete_functions;
		std::unordered_map<std::string, size_t> public_functions;
		
		while (it) {
			bool public_function = false;
			
			switch (it->get_reserved_token()) {
				case reserved_token::kw_public:
					public_function = true;
					if (!(++it)->has_value(reserved_token::kw_function)) {
						throw unexpected_syntax(it);
					}
				case reserved_token::kw_function:
					incomplete_functions.emplace_back(ctx, it);
					if (public_function) {
						public_functions.emplace(incomplete_functions.back().get_name(), incomplete_functions.size() - 1);
					}
					break;
				default:
					for (expression<lvalue>::ptr& expr : compile_variable_declaration(ctx, it)) {
						initializers.push_back(std::move(expr));
					}
					parse_token_value(ctx, it, reserved_token::semicolon);
					break;
			}
		}
		
		std::vector<lfunction> functions;
		functions.reserve(incomplete_functions.size());
		
		for (incomplete_function& f : incomplete_functions) {
			functions.emplace_back(std::make_shared<variable_impl<function> >(f.compile(ctx)));
		}
		
		return runtime_context(std::move(initializers), std::move(functions), std::move(public_functions));
	}
}
