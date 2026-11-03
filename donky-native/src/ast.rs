use crate::lexer::TokenType;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Pos { pub line: u32, pub col: u32 }

#[derive(Debug, Clone)]
pub enum Expr {
    Number(f64), String(String), Bool(bool), Ident(String),
    Unary(TokenType, Box<Expr>), Binary(Box<Expr>, TokenType, Box<Expr>),
    Call(String, Vec<Expr>, Pos), MethodCall(Box<Expr>, String, Vec<Expr>, Pos),
    Array(Vec<Expr>), Map(Vec<(Expr, Expr)>), Index(Box<Expr>, Box<Expr>),
    InterpolatedString(Vec<Expr>), StructInit(String, Vec<(String, Expr)>),
    FieldAccess(Box<Expr>, String), Function(Vec<String>, Box<Stmt>),
    Ternary(Box<Expr>, Box<Expr>, Box<Expr>),
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let(String, Expr), Assign(String, Expr), IndexSet(Box<Expr>, Box<Expr>, Box<Expr>), FieldSet(Expr, String, Expr),
    Print(Expr), ExprStmt(Expr), Block(Vec<Stmt>),
    If(Expr, Box<Stmt>, Option<Box<Stmt>>), While(Expr, Box<Stmt>),
    For(String, Expr, Box<Stmt>),
    Match(Expr, Vec<(Expr, Box<Stmt>)>, Option<Box<Stmt>>),
    Function(String, Vec<String>, Box<Stmt>), Return(Option<Expr>),
    Break(Pos), Continue(Pos), StructDef(String, Vec<(String, String)>),
    Try(Box<Stmt>, String, Box<Stmt>), Throw(Expr), Import(String), Asm(String),
    ForRange(String, Expr, Expr, Box<Stmt>),
}