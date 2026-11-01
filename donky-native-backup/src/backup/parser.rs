use crate::lexer::{Token, TokenType, Lexer};
use crate::ast::{Expr, Stmt};

pub struct Parser {
    tokens: Vec<Token>,
    current: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self { Parser { tokens, current: 0 } }
    fn peek(&self) -> &Token { &self.tokens[self.current] }
    fn peek_n(&self, n: usize) -> &Token {
        if self.current + n >= self.tokens.len() { &self.tokens[self.tokens.len() - 1] }
        else { &self.tokens[self.current + n] }
    }
    fn previous(&self) -> &Token { &self.tokens[self.current - 1] }
    fn is_at_end(&self) -> bool { self.peek().ttype == TokenType::Eof }
    fn advance(&mut self) -> &Token {
        if !self.is_at_end() { self.current += 1; }
        self.previous()
    }
    fn check(&self, ttype: TokenType) -> bool {
        if self.is_at_end() { return false; }
        self.peek().ttype == ttype
    }
    fn match_token(&mut self, types: &[TokenType]) -> bool {
        for t in types {
            if self.check(t.clone()) { self.advance(); return true; }
        }
        false
    }
    fn consume(&mut self, ttype: TokenType, message: &str) -> Result<&Token, String> {
        if self.check(ttype) { Ok(self.advance()) }
        else { Err(format!("Parse error at {}:{}: {}", self.peek().line, self.peek().col, message)) }
    }

    fn infix_binding_power(&self, ttype: &TokenType) -> Option<(u8, u8)> {
        match ttype {
            TokenType::Or => Some((1, 2)),
            TokenType::And => Some((3, 4)),
            TokenType::EqEq | TokenType::NotEq => Some((5, 6)),
            TokenType::Lt | TokenType::LtEq | TokenType::Gt | TokenType::GtEq => Some((7, 8)),
            TokenType::Plus | TokenType::Minus => Some((9, 10)),
            TokenType::Star | TokenType::Slash | TokenType::Percent | TokenType::Div => Some((11, 12)),
            _ => None,
        }
    }

    pub fn parse(&mut self) -> Result<Vec<Stmt>, String> {
        let mut statements = Vec::new();
        while !self.is_at_end() { statements.push(self.statement()?); }
        Ok(statements)
    }

    fn build_compound(&mut self, target: Expr, bin_op: TokenType, value: Expr) -> Result<Stmt, String> {
        match target {
            Expr::Ident(name) => {
                let lhs = Expr::Ident(name.clone());
                Ok(Stmt::Assign(name, Expr::Binary(Box::new(lhs), bin_op, Box::new(value))))
            }
            Expr::Index(obj, idx) => {
                if let Expr::Ident(name) = *obj {
                    let lhs = Expr::Index(Box::new(Expr::Ident(name.clone())), idx.clone());
                    Ok(Stmt::IndexSet(name, *idx, Expr::Binary(Box::new(lhs), bin_op, Box::new(value))))
                } else {
                    Err("Compound assignment only supported on simple identifiers".to_string())
                }
            }
            Expr::FieldAccess(obj, field) => {
                let lhs = Expr::FieldAccess(obj.clone(), field.clone());
                Ok(Stmt::FieldSet(*obj, field, Expr::Binary(Box::new(lhs), bin_op, Box::new(value))))
            }
            _ => Err("Invalid compound assignment target".to_string()),
        }
    }

    fn statement(&mut self) -> Result<Stmt, String> {
        if self.match_token(&[TokenType::Import]) {
            let path = self.consume(TokenType::String, "Expected path")?.literal.clone().unwrap();
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Import(path));
        }

        if self.match_token(&[TokenType::Match]) {
            let target = self.expression(0)?;
            self.consume(TokenType::LBrace, "Expected '{' after match")?;
            let mut cases = Vec::new();
            let mut else_block = None;

            while !self.check(TokenType::RBrace) && !self.is_at_end() {
                if self.match_token(&[TokenType::Ident]) && self.previous().lexeme == "_" {
                    else_block = Some(Box::new(self.statement()?));
                } else {
                    let case_val = self.expression(0)?;
                    let block = Box::new(self.statement()?);
                    cases.push((case_val, block));
                }
            }
            self.consume(TokenType::RBrace, "Expected '}'")?;
            return Ok(Stmt::Match(target, cases, else_block));
        }

        if self.match_token(&[TokenType::Try]) {
            let try_block = Box::new(self.statement()?);
            self.consume(TokenType::Catch, "Expected 'catch'")?;
            let err_name = self.consume(TokenType::Ident, "Expected error variable")?.lexeme.clone();
            let catch_block = Box::new(self.statement()?);
            return Ok(Stmt::Try(try_block, err_name, catch_block));
        }

        if self.match_token(&[TokenType::Throw]) {
            let expr = self.expression(0)?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Throw(expr));
        }

        if self.match_token(&[TokenType::Asm]) {
            self.consume(TokenType::LParen, "Expected '(' after 'asm'")?;
            let code = self.consume(TokenType::String, "Expected assembly string literal")?.literal.clone().unwrap();
            self.consume(TokenType::RParen, "Expected ')'")?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Asm(code));
        }

        if self.match_token(&[TokenType::Struct]) {
            let name = self.consume(TokenType::Ident, "Expected struct name")?.lexeme.clone();
            self.consume(TokenType::LBrace, "Expected '{'")?;
            let mut fields = Vec::new();
            while !self.check(TokenType::RBrace) && !self.is_at_end() {
                let f_name = self.consume(TokenType::Ident, "Expected field name")?.lexeme.clone();
                self.consume(TokenType::Colon, "Expected ':'")?;
                let f_type = self.consume(TokenType::Ident, "Expected type")?.lexeme.clone();
                fields.push((f_name, f_type));
                self.match_token(&[TokenType::Comma]);
            }
            self.consume(TokenType::RBrace, "Expected '}'")?;
            return Ok(Stmt::StructDef(name, fields));
        }

        if self.match_token(&[TokenType::Let]) {
            let name = self.consume(TokenType::Ident, "Expected variable name")?.lexeme.clone();
            self.consume(TokenType::Eq, "Expected '='")?;
            let value = self.expression(0)?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Let(name, value));
        }

        if self.match_token(&[TokenType::Print]) {
            self.consume(TokenType::LParen, "Expected '('")?;
            let value = self.expression(0)?;
            self.consume(TokenType::RParen, "Expected ')'")?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Print(value));
        }

        if self.match_token(&[TokenType::LBrace]) {
            let mut stmts = Vec::new();
            while !self.check(TokenType::RBrace) && !self.is_at_end() { stmts.push(self.statement()?); }
            self.consume(TokenType::RBrace, "Expected '}'")?;
            return Ok(Stmt::Block(stmts));
        }

        if self.match_token(&[TokenType::If]) {
            let cond = self.expression(0)?;
            let then_branch = Box::new(self.statement()?);
            let mut else_branch = None;
            if self.match_token(&[TokenType::Else]) { else_branch = Some(Box::new(self.statement()?)); }
            return Ok(Stmt::If(cond, then_branch, else_branch));
        }

        if self.match_token(&[TokenType::While]) {
            let cond = self.expression(0)?;
            let body = Box::new(self.statement()?);
            return Ok(Stmt::While(cond, body));
        }

        if self.match_token(&[TokenType::For]) {
            let name = self.consume(TokenType::Ident, "Expected loop variable")?.lexeme.clone();
            self.consume(TokenType::In, "Expected 'in'")?;
            let iter_expr = self.expression(0)?;
            let body = Box::new(self.statement()?);
            return Ok(Stmt::For(name, iter_expr, body));
        }

        if self.match_token(&[TokenType::Fn]) {
            let mut name = self.consume(TokenType::Ident, "Expected function name")?.lexeme.clone();
            if self.match_token(&[TokenType::Dot]) {
                let method = self.consume(TokenType::Ident, "Expected method name")?.lexeme.clone();
                name = format!("{}.{}", name, method);
            }
            self.consume(TokenType::LParen, "Expected '('")?;
            let mut params = Vec::new();
            if !self.check(TokenType::RParen) {
                loop {
                    params.push(self.consume(TokenType::Ident, "Expected parameter name")?.lexeme.clone());
                    if !self.match_token(&[TokenType::Comma]) { break; }
                }
            }
            self.consume(TokenType::RParen, "Expected ')'")?;
            self.match_token(&[TokenType::Minus]); self.match_token(&[TokenType::Gt]);
            if self.check(TokenType::Ident) { self.advance(); }
            let body = Box::new(self.statement()?);
            return Ok(Stmt::Function(name, params, body));
        }

        if self.match_token(&[TokenType::Return]) {
            if self.match_token(&[TokenType::Semi]) { return Ok(Stmt::Return(None)); }
            let value = self.expression(0)?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Return(Some(value)));
        }

        if self.match_token(&[TokenType::Break]) { self.consume(TokenType::Semi, "Expected ';'")?; return Ok(Stmt::Break); }
        if self.match_token(&[TokenType::Continue]) { self.consume(TokenType::Semi, "Expected ';'")?; return Ok(Stmt::Continue); }

        if self.check(TokenType::Ident) && self.peek_n(1).ttype == TokenType::PlusPlus {
            let name = self.advance().lexeme.clone();
            self.advance();
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Assign(name.clone(), Expr::Binary(Box::new(Expr::Ident(name)), TokenType::Plus, Box::new(Expr::Number(1.0)))));
        }
        if self.check(TokenType::Ident) && self.peek_n(1).ttype == TokenType::MinusMinus {
            let name = self.advance().lexeme.clone();
            self.advance();
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Assign(name.clone(), Expr::Binary(Box::new(Expr::Ident(name)), TokenType::Minus, Box::new(Expr::Number(1.0)))));
        }

        if self.check(TokenType::Ident) {
            let next = self.peek_n(1).ttype.clone();
            if next == TokenType::PlusEq || next == TokenType::MinusEq || next == TokenType::StarEq || next == TokenType::SlashEq {
                let name = self.advance().lexeme.clone();
                let op_token = self.advance().ttype.clone();
                let value = self.expression(0)?;
                self.consume(TokenType::Semi, "Expected ';'")?;
                let bin_op = match op_token {
                    TokenType::PlusEq => TokenType::Plus,
                    TokenType::MinusEq => TokenType::Minus,
                    TokenType::StarEq => TokenType::Star,
                    TokenType::SlashEq => TokenType::Slash,
                    _ => unreachable!(),
                };
                return Ok(Stmt::Assign(name.clone(), Expr::Binary(Box::new(Expr::Ident(name)), bin_op, Box::new(value))));
            }
        }

        if self.check(TokenType::Ident) && self.peek_n(1).ttype == TokenType::Eq {
            let name = self.advance().lexeme.clone();
            self.advance();
            let value = self.expression(0)?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            return Ok(Stmt::Assign(name, value));
        }

        let expr = self.expression(0)?;

        if self.check(TokenType::PlusEq) || self.check(TokenType::MinusEq) || self.check(TokenType::StarEq) || self.check(TokenType::SlashEq) {
            let op_token = self.advance().ttype.clone();
            let value = self.expression(0)?;
            let bin_op = match op_token {
                TokenType::PlusEq => TokenType::Plus,
                TokenType::MinusEq => TokenType::Minus,
                TokenType::StarEq => TokenType::Star,
                TokenType::SlashEq => TokenType::Slash,
                _ => unreachable!(),
            };
            self.consume(TokenType::Semi, "Expected ';'")?;
            return self.build_compound(expr, bin_op, value);
        }

        if self.check(TokenType::PlusPlus) || self.check(TokenType::MinusMinus) {
            let is_plus = self.check(TokenType::PlusPlus);
            self.advance();
            let bin_op = if is_plus { TokenType::Plus } else { TokenType::Minus };
            self.consume(TokenType::Semi, "Expected ';'")?;
            return self.build_compound(expr, bin_op, Expr::Number(1.0));
        }

        if self.match_token(&[TokenType::Eq]) {
            let value = self.expression(0)?;
            self.consume(TokenType::Semi, "Expected ';'")?;
            match expr {
                Expr::Index(target, index) => {
                    if let Expr::Ident(name) = *target { return Ok(Stmt::IndexSet(name, *index, value)); }
                    return Err("Invalid array assignment target".to_string());
                }
                Expr::FieldAccess(obj, field) => { return Ok(Stmt::FieldSet(*obj, field, value)); }
                _ => return Err("Invalid assignment target".to_string()),
            }
        }

        self.consume(TokenType::Semi, "Expected ';' after expression")?;
        Ok(Stmt::ExprStmt(expr))
    }

    fn expression(&mut self, min_bp: u8) -> Result<Expr, String> {
        let mut left = self.prefix()?;

        while self.match_token(&[TokenType::LBracket]) {
            let index = self.expression(0)?;
            self.consume(TokenType::RBracket, "Expected ']'")?;
            left = Expr::Index(Box::new(left), Box::new(index));
        }

        while self.match_token(&[TokenType::Dot]) {
            let field = self.consume(TokenType::Ident, "Expected field name")?.lexeme.clone();
            if self.match_token(&[TokenType::LParen]) {
                let mut args = Vec::new();
                if !self.check(TokenType::RParen) {
                    loop {
                        args.push(self.expression(0)?);
                        if !self.match_token(&[TokenType::Comma]) { break; }
                    }
                }
                self.consume(TokenType::RParen, "Expected ')'")?;
                left = Expr::MethodCall(Box::new(left), field, args);
            } else {
                left = Expr::FieldAccess(Box::new(left), field);
            }
        }

        while let Some((l_bp, r_bp)) = self.infix_binding_power(&self.peek().ttype) {
            if l_bp < min_bp { break; }
            let op_token = self.advance().ttype.clone();
            let right = self.expression(r_bp)?;
            left = Expr::Binary(Box::new(left), op_token, Box::new(right));
        }

        if min_bp <= 2 && self.match_token(&[TokenType::Question]) {
            let then_branch = self.expression(0)?;
            self.consume(TokenType::Colon, "Expected ':' in ternary")?;
            let else_branch = self.expression(2)?;
            left = Expr::Ternary(Box::new(left), Box::new(then_branch), Box::new(else_branch));
        }

        Ok(left)
    }

    fn prefix(&mut self) -> Result<Expr, String> {
        let token = self.advance().clone();
        match token.ttype {
            TokenType::Number => Ok(Expr::Number(token.literal.unwrap().parse().unwrap())),
            TokenType::String => Ok(Expr::String(token.literal.unwrap())),
            TokenType::StringInterp => {
                let raw = token.literal.unwrap();
                let mut parts = Vec::new();
                let mut current_text = String::new();
                let mut chars = raw.chars().peekable();
                while let Some(c) = chars.next() {
                    if c == '$' && chars.peek() == Some(&'{') {
                        chars.next();
                        if !current_text.is_empty() { parts.push(Expr::String(current_text.clone())); current_text.clear(); }
                        let mut expr_text = String::new();
                        let mut depth = 1;
                        while let Some(&nc) = chars.peek() {
                            if nc == '{' { depth += 1; }
                            if nc == '}' { depth -= 1; }
                            if depth == 0 { break; }
                            expr_text.push(chars.next().unwrap());
                        }
                        chars.next();
                        let mut sub_lex = Lexer::new(&expr_text);
                        let sub_tokens = sub_lex.tokenize()?;
                        let mut sub_parser = Parser::new(sub_tokens);
                        parts.push(sub_parser.expression(0)?);
                    } else { current_text.push(c); }
                }
                if !current_text.is_empty() { parts.push(Expr::String(current_text)); }
                Ok(Expr::InterpolatedString(parts))
            }
            TokenType::True => Ok(Expr::Bool(true)),
            TokenType::False => Ok(Expr::Bool(false)),
            // NEW: anonymous functions in expression position
            TokenType::Fn => {
                self.consume(TokenType::LParen, "Expected '(' after 'fn'")?;
                let mut params = Vec::new();
                if !self.check(TokenType::RParen) {
                    loop {
                        params.push(self.consume(TokenType::Ident, "Expected parameter name")?.lexeme.clone());
                        if !self.match_token(&[TokenType::Comma]) { break; }
                    }
                }
                self.consume(TokenType::RParen, "Expected ')'")?;
                self.match_token(&[TokenType::Minus]); self.match_token(&[TokenType::Gt]);
                if self.check(TokenType::Ident) { self.advance(); }
                let body = Box::new(self.statement()?);
                Ok(Expr::Function(params, body))
            }
            TokenType::Ident => {
                let name = token.lexeme;
                if self.match_token(&[TokenType::LParen]) {
                    let mut args = Vec::new();
                    if !self.check(TokenType::RParen) {
                        loop {
                            args.push(self.expression(0)?);
                            if !self.match_token(&[TokenType::Comma]) { break; }
                        }
                    }
                    self.consume(TokenType::RParen, "Expected ')'")?;
                    Ok(Expr::Call(name, args))
                } else if self.check(TokenType::LBrace) && self.peek_n(1).ttype == TokenType::Ident && self.peek_n(2).ttype == TokenType::Colon {
                    self.advance();
                    let mut fields = Vec::new();
                    while !self.check(TokenType::RBrace) && !self.is_at_end() {
                        let f_name = self.consume(TokenType::Ident, "Expected field name")?.lexeme.clone();
                        self.consume(TokenType::Colon, "Expected ':'")?;
                        let f_val = self.expression(0)?;
                        fields.push((f_name, f_val));
                        self.match_token(&[TokenType::Comma]);
                    }
                    self.consume(TokenType::RBrace, "Expected '}'")?;
                    Ok(Expr::StructInit(name, fields))
                } else {
                    Ok(Expr::Ident(name))
                }
            }
            TokenType::LParen => {
                let expr = self.expression(0)?;
                self.consume(TokenType::RParen, "Expected ')'")?;
                Ok(expr)
            }
            TokenType::Minus | TokenType::Bang => {
                let operand = self.expression(13)?;
                Ok(Expr::Unary(token.ttype, Box::new(operand)))
            }
            TokenType::LBracket => {
                let mut elements = Vec::new();
                if !self.check(TokenType::RBracket) {
                    loop {
                        elements.push(self.expression(0)?);
                        if !self.match_token(&[TokenType::Comma]) { break; }
                    }
                }
                self.consume(TokenType::RBracket, "Expected ']'")?;
                Ok(Expr::Array(elements))
            }
            TokenType::LBrace => {
                let mut pairs = Vec::new();
                if !self.check(TokenType::RBrace) {
                    loop {
                        let key = self.expression(0)?;
                        self.consume(TokenType::Colon, "Expected ':'")?;
                        let value = self.expression(0)?;
                        pairs.push((key, value));
                        if !self.match_token(&[TokenType::Comma]) { break; }
                    }
                }
                self.consume(TokenType::RBrace, "Expected '}'")?;
                Ok(Expr::Map(pairs))
            }
            _ => Err(format!("Unexpected token: {:?}", token.ttype)),
        }
    }
}