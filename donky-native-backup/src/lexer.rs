#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub enum TokenType {
    Number, String, StringInterp, Ident,
    Let, Fn, If, Else, While, For, In, Return, Print, True, False, Div, Break, Continue, Try, Catch, Throw, Import, Struct, Asm, Match,
    Plus, Minus, Star, Slash, Percent, Bang, And, Or, Eq, EqEq, NotEq, Lt, LtEq, Gt, GtEq,
    PlusEq, MinusEq, StarEq, SlashEq, PlusPlus, MinusMinus, Question,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semi, Comma, Colon, Dot, Eof,
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

    pub fn tokenize(&mut self) -> Result<Vec<Token>, String> {
        let mut tokens = Vec::new();

        while !self.at_end() {
            let ch = self.peek(0);
            
            if ch.is_whitespace() { self.advance(); continue; }
            if ch == '/' && self.peek(1) == '/' {
                while self.peek(0) != '\n' && !self.at_end() { self.advance(); }
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
                ':' => TokenType::Colon, '.' => TokenType::Dot,
                '?' => TokenType::Question,
                '+' => if self.match_char('=') { TokenType::PlusEq } else if self.match_char('+') { TokenType::PlusPlus } else { TokenType::Plus },
                '-' => if self.match_char('=') { TokenType::MinusEq } else if self.match_char('-') { TokenType::MinusMinus } else { TokenType::Minus },
                '*' => if self.match_char('=') { TokenType::StarEq } else { TokenType::Star },
                '/' => if self.match_char('=') { TokenType::SlashEq } else { TokenType::Slash },
                '%' => TokenType::Percent,
                '=' => if self.match_char('=') { TokenType::EqEq } else { TokenType::Eq },
                '!' => if self.match_char('=') { TokenType::NotEq } else { TokenType::Bang },
                '<' => if self.match_char('=') { TokenType::LtEq } else { TokenType::Lt },
                '>' => if self.match_char('=') { TokenType::GtEq } else { TokenType::Gt },
                '&' => { if self.match_char('&') { TokenType::And } else { return Err("Unexpected &".into()); } },
                '|' => { if self.match_char('|') { TokenType::Or } else { return Err("Unexpected |".into()); } },
                '"' => {
                    let mut val = String::new();
                    let mut has_interp = false;
                    while !self.at_end() && self.peek(0) != '"' {
                        let next_c = self.advance();
                        if next_c == '\\' {
                            let esc = self.advance();
                            if esc == 'n' { val.push('\n'); }
                            else if esc == 't' { val.push('\t'); }
                            else if esc == 'r' { val.push('\r'); }
                            else { val.push(esc); }
                        } else if next_c == '$' && self.peek(0) == '{' {
                            has_interp = true;
                            val.push('$');
                            val.push('{');
                            self.advance();
                        } else {
                            val.push(next_c);
                        }
                    }
                    if self.at_end() { return Err("Unterminated string".into()); }
                    self.advance();
                    
                    let ttype = if has_interp { TokenType::StringInterp } else { TokenType::String };
                    tokens.push(Token { ttype, lexeme: format!("\"{}\"", val), literal: Some(val), line: start_line, col: start_col });
                    continue;
                },
                _ => {
                    if c.is_ascii_digit() {
                        let mut num_str = c.to_string();
                        while self.peek(0).is_ascii_digit() || self.peek(0) == '.' {
                            num_str.push(self.advance());
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
                        return Err(format!("Unexpected character: {}", c));
                    }
                }
            };
            tokens.push(Token { ttype: token_type, lexeme: c.to_string(), literal: None, line: start_line, col: start_col });
        }

        tokens.push(Token { ttype: TokenType::Eof, lexeme: String::new(), literal: None, line: self.line, col: self.col });
        Ok(tokens)
    }
}