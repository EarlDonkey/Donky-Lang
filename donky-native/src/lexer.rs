#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub enum TokenType {
    Number, String, StringInterp, Ident,
    Let, Fn, If, Else, While, For, In, Return, Print, True, False, Div, Break, Continue, Try, Catch, Throw, Import, Struct, Asm, Match,
    Plus, Minus, Star, Slash, Percent, Bang, And, Or, Eq, EqEq, NotEq, Lt, LtEq, Gt, GtEq,
    PlusEq, MinusEq, StarEq, SlashEq, PlusPlus, MinusMinus, Question,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semi, Comma, Colon, Dot, Eof, PercentEq, DotDot,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub ttype: TokenType,
    pub lexeme: String,
    pub literal: Option<String>,
    pub line: usize,
    pub col: usize,
}

pub struct Lexer {
    source: Vec<char>,
    pos: usize,
    line: usize,
    col: usize,
}

fn lex_err(line: usize, col: usize, msg: &str) -> String {
    format!("Lex error at {}:{}: {}", line, col, msg)
}

impl Lexer {
    pub fn new(source: &str) -> Self {
        let src = source.trim_start_matches('\u{feff}');
        Lexer { source: src.chars().collect(), pos: 0, line: 1, col: 1 }
    }

    fn at_end(&self) -> bool { self.pos >= self.source.len() }

    fn peek(&self, offset: usize) -> char {
        if self.pos + offset >= self.source.len() { '\0' } else { self.source[self.pos + offset] }
    }

    fn advance(&mut self) -> char {
        let ch = self.source[self.pos];
        self.pos += 1;
        if ch == '\n' { self.line += 1; self.col = 1; } else { self.col += 1; }
        ch
    }

    fn match_char(&mut self, expected: char) -> bool {
        if self.at_end() || self.source[self.pos] != expected { return false; }
        self.advance();
        true
    }

    /// Copies the raw text of a `${ ... }` expression (the opening `${` is already consumed)
    /// up to and including the matching `}`. Quoted strings inside the expression are copied
    /// verbatim, so `"a ${m["k"]}"` and `"${f("}")}"` work. The sub-lexer re-lexes this text.
    fn copy_interp_body(&mut self, out: &mut String, line: usize, col: usize) -> Result<(), String> {
        let mut depth = 1usize;
        while !self.at_end() {
            let c = self.advance();
            out.push(c);
            match c {
                '{' => depth += 1,
                '}' => { depth -= 1; if depth == 0 { return Ok(()); } }
                '"' => self.copy_nested_string(out, line, col)?,
                _ => {}
            }
        }
        Err(lex_err(line, col, "Unterminated ${...} in string"))
    }

    /// Copies a nested string literal (opening quote already consumed) through its closing quote.
    fn copy_nested_string(&mut self, out: &mut String, line: usize, col: usize) -> Result<(), String> {
        while !self.at_end() {
            let c = self.advance();
            out.push(c);
            match c {
                '\\' => { if !self.at_end() { let e = self.advance(); out.push(e); } }
                '"' => return Ok(()),
                '$' if self.peek(0) == '{' => {
                    let b = self.advance();
                    out.push(b);
                    self.copy_interp_body(out, line, col)?;
                }
                _ => {}
            }
        }
        Err(lex_err(line, col, "Unterminated string inside ${...}"))
    }

    pub fn tokenize(&mut self) -> Result<Vec<Token>, String> {
        let mut tokens = Vec::new();

        while !self.at_end() {
            let ch = self.peek(0);

            if ch.is_whitespace() { self.advance(); continue; }
            if ch == '/' && self.peek(1) == '/' {
                while self.peek(0) != '\n' && !self.at_end() { self.advance(); }
                continue;
            }
            if ch == '/' && self.peek(1) == '*' {
                let (sl, sc) = (self.line, self.col);
                self.advance(); self.advance();
                loop {
                    if self.at_end() { return Err(lex_err(sl, sc, "Unterminated block comment")); }
                    if self.peek(0) == '*' && self.peek(1) == '/' { self.advance(); self.advance(); break; }
                    self.advance();
                }
                continue;
            }

            let start_line = self.line;
            let start_col = self.col;
            let c = self.advance();

            let token_type = match c {
                '(' => TokenType::LParen, ')' => TokenType::RParen,
                '{' => TokenType::LBrace, '}' => TokenType::RBrace,
                '[' => TokenType::LBracket, ']' => TokenType::RBracket,
                ';' => TokenType::Semi, ',' => TokenType::Comma,
                ':' => TokenType::Colon,
                '?' => TokenType::Question,
                '+' => if self.match_char('=') { TokenType::PlusEq } else if self.match_char('+') { TokenType::PlusPlus } else { TokenType::Plus },
                '-' => if self.match_char('=') { TokenType::MinusEq } else if self.match_char('-') { TokenType::MinusMinus } else { TokenType::Minus },
                '*' => if self.match_char('=') { TokenType::StarEq } else { TokenType::Star },
                '/' => if self.match_char('=') { TokenType::SlashEq } else { TokenType::Slash },
                '.' => {
                    if self.match_char('.') { TokenType::DotDot } else { TokenType::Dot }
                }
                '%' => {
                    if self.match_char('=') { TokenType::PercentEq } else { TokenType::Percent }
                }
                '=' => if self.match_char('=') { TokenType::EqEq } else { TokenType::Eq },
                '!' => if self.match_char('=') { TokenType::NotEq } else { TokenType::Bang },
                '<' => if self.match_char('=') { TokenType::LtEq } else { TokenType::Lt },
                '>' => if self.match_char('=') { TokenType::GtEq } else { TokenType::Gt },
                '&' => { if self.match_char('&') { TokenType::And } else { return Err(lex_err(start_line, start_col, "Unexpected '&' (did you mean '&&'?)")); } },
                '|' => { if self.match_char('|') { TokenType::Or } else { return Err(lex_err(start_line, start_col, "Unexpected '|' (did you mean '||'?)")); } },
                '`' => {
                    let mut val = String::new();
                    while !self.at_end() && self.peek(0) != '`' { val.push(self.advance()); }
                    if self.at_end() { return Err(lex_err(start_line, start_col, "Unterminated raw string")); }
                    self.advance();
                    tokens.push(Token { ttype: TokenType::String, lexeme: format!("`{}`", val), literal: Some(val), line: start_line, col: start_col });
                    continue;
                },
                '"' => {
                    let mut val = String::new();
                    let mut has_interp = false;
                    loop {
                        if self.at_end() { return Err(lex_err(start_line, start_col, "Unterminated string")); }
                        let next_c = self.advance();
                        match next_c {
                            '"' => break,
                            '\\' => {
                                // A trailing backslash used to index past the end of the source and panic.
                                if self.at_end() { return Err(lex_err(start_line, start_col, "Unterminated string")); }
                                let esc = self.advance();
                                match esc {
                                    'n' => val.push('\n'),
                                    't' => val.push('\t'),
                                    'r' => val.push('\r'),
                                    other => val.push(other),
                                }
                            }
                            '$' if self.peek(0) == '{' => {
                                has_interp = true;
                                val.push('$');
                                val.push('{');
                                self.advance();
                                self.copy_interp_body(&mut val, start_line, start_col)?;
                            }
                            other => val.push(other),
                        }
                    }

                    let ttype = if has_interp { TokenType::StringInterp } else { TokenType::String };
                    tokens.push(Token { ttype, lexeme: format!("\"{}\"", val), literal: Some(val), line: start_line, col: start_col });
                    continue;
                },
                _ => {
                    if c.is_ascii_digit() {
                        // Hex literal: 0xFF (stored as its decimal value so the parser needs no changes)
                        if c == '0' && (self.peek(0) == 'x' || self.peek(0) == 'X') && self.peek(1).is_ascii_hexdigit() {
                            self.advance();
                            let mut hex = String::new();
                            while self.peek(0).is_ascii_hexdigit() || (self.peek(0) == '_' && self.peek(1).is_ascii_hexdigit()) {
                                let h = self.advance();
                                if h != '_' { hex.push(h); }
                            }
                            let n = u64::from_str_radix(&hex, 16)
                                .map_err(|_| lex_err(start_line, start_col, "Hex literal too large"))?;
                            tokens.push(Token { ttype: TokenType::Number, lexeme: format!("0x{}", hex), literal: Some((n as f64).to_string()), line: start_line, col: start_col });
                            continue;
                        }
                        let mut num_str = c.to_string();
                        let mut seen_dot = false;
                        loop {
                            let p = self.peek(0);
                            if p.is_ascii_digit() {
                                num_str.push(self.advance());
                            } else if p == '_' && self.peek(1).is_ascii_digit() {
                                self.advance(); // digit separator: 1_000_000
                            } else if p == '.' && !seen_dot && self.peek(1).is_ascii_digit() {
                                seen_dot = true;
                                num_str.push(self.advance());
                            } else {
                                break;
                            }
                        }
                        // Exponent: 1e9, 2.5E-3 (only if digits actually follow, so `1else` style typos stay errors elsewhere)
                        let e = self.peek(0);
                        if e == 'e' || e == 'E' {
                            let sign = self.peek(1);
                            if self.peek(1).is_ascii_digit() || ((sign == '+' || sign == '-') && self.peek(2).is_ascii_digit()) {
                                num_str.push(self.advance());
                                if sign == '+' || sign == '-' { num_str.push(self.advance()); }
                                while self.peek(0).is_ascii_digit() { num_str.push(self.advance()); }
                            }
                        }
                        tokens.push(Token { ttype: TokenType::Number, lexeme: num_str.clone(), literal: Some(num_str), line: start_line, col: start_col });
                        continue;
                    } else if c.is_alphabetic() || c == '_' {
                        let mut ident = c.to_string();
                        while self.peek(0).is_alphanumeric() || self.peek(0) == '_' {
                            ident.push(self.advance());
                        }
                        let ttype = match ident.as_str() {
                            "let" => TokenType::Let, "fn" => TokenType::Fn, "if" => TokenType::If,
                            "else" => TokenType::Else, "while" => TokenType::While, "for" => TokenType::For,
                            "in" => TokenType::In, "return" => TokenType::Return, "print" => TokenType::Print,
                            "true" => TokenType::True, "false" => TokenType::False, "div" => TokenType::Div,
                            "break" => TokenType::Break, "continue" => TokenType::Continue, "try" => TokenType::Try,
                            "catch" => TokenType::Catch, "throw" => TokenType::Throw, "import" => TokenType::Import,
                            "struct" => TokenType::Struct, "asm" => TokenType::Asm, "match" => TokenType::Match,
                            _ => TokenType::Ident,
                        };
                        tokens.push(Token { ttype, lexeme: ident, literal: None, line: start_line, col: start_col });
                        continue;
                    } else {
                        return Err(lex_err(start_line, start_col, &format!("Unexpected character '{}'", c)));
                    }
                }
            };
            tokens.push(Token { ttype: token_type, lexeme: c.to_string(), literal: None, line: start_line, col: start_col });
        }

        tokens.push(Token { ttype: TokenType::Eof, lexeme: String::new(), literal: None, line: self.line, col: self.col });
        Ok(tokens)
    }
}
