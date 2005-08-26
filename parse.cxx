// recursive descent parser for systemtap scripts
// Copyright (C) 2005 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "parse.h"
#include <iostream>
#include <fstream>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <climits>

using namespace std;

// ------------------------------------------------------------------------



parser::parser (istream& i, bool p):
  input_name ("<input>"), free_input (0),
  input (i, input_name), privileged (p),
  last_t (0), next_t (0), num_errors (0)
{ }

parser::parser (const string& fn, bool p):
  input_name (fn), free_input (new ifstream (input_name.c_str(), ios::in)),
  input (* free_input, input_name), privileged (p),
  last_t (0), next_t (0), num_errors (0)
{ }

parser::~parser()
{
  if (free_input) delete free_input;
}


stapfile*
parser::parse (std::istream& i, bool pr)
{
  parser p (i, pr);
  return p.parse ();
}


stapfile*
parser::parse (const std::string& n, bool pr)
{
  parser p (n, pr);
  return p.parse ();
}

static string
tt2str(token_type tt)
{
  switch (tt)
    {
    case tok_junk: return "junk";
    case tok_identifier: return "identifier";
    case tok_operator: return "operator";
    case tok_string: return "string";
    case tok_number: return "number";
    case tok_embedded: return "embedded-code";
    }
  return "unknown token";
}

ostream&
operator << (ostream& o, const token& t)
{
  o << tt2str(t.type);

  if (t.type != tok_embedded) // XXX: other types?
    {
      o << " '";
      for (unsigned i=0; i<t.content.length(); i++)
        {
          char c = t.content[i];
          o << (isprint (c) ? c : '?');
        }
      o << "'";
    }

  o << " at " 
    << t.location.file << ":" 
    << t.location.line << ":"
    << t.location.column;

  return o;
}


void 
parser::print_error  (const parse_error &pe)
{
  cerr << "parse error: " << pe.what () << endl;

  const token* t = last_t;
  if (t)
    cerr << "\tsaw: " << *t << endl;
  else
    cerr << "\tsaw: " << input_name << " EOF" << endl;

  // XXX: make it possible to print the last input line,
  // so as to line up an arrow with the specific error column

  num_errors ++;
}


const token* 
parser::last ()
{
  return last_t;
}


const token*
parser::next ()
{
  if (! next_t)
    next_t = input.scan ();
  if (! next_t)
    throw parse_error ("unexpected end-of-file");

  last_t = next_t;
  // advance by zeroing next_t
  next_t = 0;
  return last_t;
}


const token*
parser::peek ()
{
  if (! next_t)
    next_t = input.scan ();

  // cerr << "{" << (next_t ? next_t->content : "null") << "}";

  // don't advance by zeroing next_t
  last_t = next_t;
  return next_t;
}


static inline bool
tok_is(token const * t, token_type tt, string const & expected)
{
  return t && t->type == tt && t->content == expected;
}


const token* 
parser::expect_known (token_type tt, string const & expected)
{
  const token *t = next();
  if (! t && t->type == tt && t->content == expected)
    throw parse_error ("expected '" + expected + "'");
  return t;
}


const token* 
parser::expect_unknown (token_type tt, string & target)
{
  const token *t = next();
  if (!(t && t->type == tt))
    throw parse_error ("expected " + tt2str(tt));
  target = t->content;
  return t;
}


const token* 
parser::expect_op (std::string const & expected)
{
  return expect_known (tok_operator, expected);
}


const token* 
parser::expect_kw (std::string const & expected)
{
  return expect_known (tok_identifier, expected);
}


const token* 
parser::expect_ident (std::string & target)
{
  return expect_unknown (tok_identifier, target);
}


bool 
parser::peek_op (std::string const & op)
{
  return tok_is (peek(), tok_operator, op);
}


bool 
parser::peek_kw (std::string const & kw)
{
  return tok_is (peek(), tok_identifier, kw);
}



lexer::lexer (istream& i, const string& in):
  input (i), input_name (in), cursor_line (1), cursor_column (1)
{ }


int
lexer::input_peek (unsigned n)
{
  while (lookahead.size() <= n)
    {
      int c = input.get ();
      lookahead.push_back (input ? c : -1);
    }
  return lookahead[n];
}


int 
lexer::input_get ()
{
  int c = input_peek (0);
  lookahead.erase (lookahead.begin ());

  if (c < 0) return c; // EOF

  // update source cursor
  if (c == '\n')
    {
      cursor_line ++;
      cursor_column = 1;
    }
  else
    cursor_column ++;

  return c;
}


token*
lexer::scan ()
{
  token* n = new token;
  n->location.file = input_name;

 skip:
  n->location.line = cursor_line;
  n->location.column = cursor_column;

  int c = input_get();
  if (c < 0)
    {
      delete n;
      return 0;
    }

  if (isspace (c))
    goto skip;

  else if (isalpha (c) || c == '$' || c == '_')
    {
      n->type = tok_identifier;
      n->content = (char) c;
      while (1)
	{
	  int c2 = input_peek ();
	  if (! input)
	    break;
	  if ((isalnum(c2) || c2 == '_' || c2 == '$'))
	    {
	      n->content.push_back(c2);
	      input_get ();
	    }
	  else
	    break;
	}
      return n;
    }

  else if (isdigit (c)) // positive literal
    {
      n->type = tok_number;
      n->content = (char) c;

      while (1)
	{
	  int c2 = input_peek ();
	  if (! input)
	    break;

          // NB: isalnum is very permissive.  We rely on strtol, called in
          // parser::parse_literal below, to confirm that the number string
          // is correctly formatted and in range.

	  if (isalnum (c2))
	    {
	      n->content.push_back (c2);
	      input_get ();
	    }
	  else
	    break;
	}
      return n;
    }

  else if (c == '\"')
    {
      n->type = tok_string;
      while (1)
	{
	  c = input_get ();

	  if (! input || c == '\n')
	    {
	      n->type = tok_junk;
	      break;
	    }
	  if (c == '\"') // closing double-quotes
	    break;
	  else if (c == '\\')
	    {	      
	      c = input_get ();
	      switch (c)
		{
		case 'a':
		case 'b':
		case 't':
		case 'n':
		case 'v':
		case 'f':
		case 'r':
		case '\\':

		  // Pass these escapes through to the string value
		  // beign parsed; it will "likely" be emitted into 
		  // a C literal. 
		  //
		  // XXX: verify this assumption.

		  n->content.push_back('\\');

		default:

		  n->content.push_back(c);
		  break;
		}
	    }
	  else
	    n->content.push_back(c);
	}
      return n;
    }

  else if (ispunct (c))
    {
      int c2 = input_peek ();
      int c3 = input_peek (1);
      string s1 = string("") + (char) c;
      string s2 = (c2 > 0 ? s1 + (char) c2 : s1);
      string s3 = (c3 > 0 ? s2 + (char) c3 : s2);

      // NB: if we were to recognize negative numeric literals here,
      // we'd introduce another grammar ambiguity:
      // 1-1 would be parsed as tok_number(1) and tok_number(-1)
      // instead of tok_number(1) tok_operator('-') tok_number(1)

      if (s1 == "#") // shell comment
        {
          unsigned this_line = cursor_line;
          do { c = input_get (); }
          while (c >= 0 && cursor_line == this_line);
          goto skip;
        }
      else if (s2 == "//") // C++ comment
        {
          unsigned this_line = cursor_line;
          do { c = input_get (); }
          while (c >= 0 && cursor_line == this_line);
          goto skip;
        }
      else if (c == '/' && c2 == '*') // C comment
	{
          c2 = input_get ();
          unsigned chars = 0;
          while (c2 >= 0)
            {
              chars ++; // track this to prevent "/*/" from being accepted
              c = c2;
              c2 = input_get ();
              if (chars > 1 && c == '*' && c2 == '/')
                break;
            }
          goto skip;
	}
      else if (c == '%' && c2 == '{') // embedded code
        {
          n->type = tok_embedded;
          (void) input_get (); // swallow '{' already in c2
          while (true)
            {
              c = input_get ();
              if (c == 0) // EOF
                {
                  n->type = tok_junk;
                  break;
                }
              if (c == '%')
                {
                  c2 = input_peek ();
                  if (c2 == '}')
                    {
                      (void) input_get (); // swallow '}' too
                      break;
                    }
                }
              n->content += c;
            }
          return n;
        }

      // We're committed to recognizing at least the first character
      // as an operator.
      n->type = tok_operator;

      // match all valid operators, in decreasing size order
      if (s3 == "<<<" ||
          s3 == "<<=" ||
          s3 == ">>=")
        {
          n->content = s3;
          input_get (); input_get (); // swallow other two characters
        }
      else if (s2 == "==" ||
               s2 == "!=" ||
               s2 == "<=" ||
               s2 == ">=" ||
               s2 == "+=" ||
               s2 == "-=" ||
               s2 == "*=" ||
               s2 == "/=" ||
               s2 == "%=" ||
               s2 == "&=" ||
               s2 == "^=" ||
               s2 == "|=" ||
               s2 == ".=" ||
               s2 == "&&" ||
               s2 == "||" ||
               s2 == "++" ||
               s2 == "--" ||
               s2 == "->" ||
               s2 == "<<" ||
               s2 == ">>")
        {
          n->content = s2;
          input_get (); // swallow other character
        }   
      else
        {
          n->content = s1;
        }

      return n;
    }

  else
    {
      n->type = tok_junk;
      n->content = (char) c;
      return n;
    }
}


// ------------------------------------------------------------------------

stapfile*
parser::parse ()
{
  stapfile* f = new stapfile;
  f->name = input_name;

  bool empty = true;

  while (1)
    {
      try
	{
	  const token* t = peek ();
	  if (! t) // nice clean EOF
	    break;

          empty = false;
	  if (t->type == tok_identifier && t->content == "probe")
            parse_probe (f->probes, f->aliases);
	  else if (t->type == tok_identifier && t->content == "global")
	    parse_global (f->globals);
	  else if (t->type == tok_identifier && t->content == "function")
            parse_functiondecl (f->functions);
          else if (t->type == tok_embedded)
            f->embeds.push_back (parse_embeddedcode ());
	  else
	    throw parse_error ("expected 'probe', 'global', 'function', or '%{'");
	}
      catch (parse_error& pe)
	{
	  print_error (pe);
	  // Quietly swallow all tokens until the next '}'.
	  while (1)
	    {
	      const token* t = peek ();
	      if (! t)
		break;
	      next ();
	      if (t->type == tok_operator && t->content == "}")
		break;
	    }
	}
    }

  if (empty)
    {
      cerr << "Input file '" << input_name << "' is empty or missing." << endl;
      delete f;
      return 0;
    }
  else if (num_errors > 0)
    {
      cerr << num_errors << " parse error(s)." << endl;
      delete f;
      return 0;
    }
  
  return f;
}


void
parser::parse_probe (std::vector<probe *> & probe_ret,
		     std::vector<probe_alias *> & alias_ret)
{
  const token* t0 = next ();
  if (! (t0->type == tok_identifier && t0->content == "probe"))
    throw parse_error ("expected 'probe'");

  vector<probe_point *> aliases;
  vector<probe_point *> locations;

  bool equals_ok = true;

  while (1)
    {
      const token *t = peek ();
      if (t && t->type == tok_identifier)
	{
	  probe_point * pp = parse_probe_point ();

	  t = peek ();
	  if (equals_ok && t 
	      && t->type == tok_operator && t->content == "=")
	    {
	      aliases.push_back(pp);
	      next ();
	      continue;
	    }
	  else if (t && t->type == tok_operator && t->content == ",")
	    {
	      locations.push_back(pp);
	      equals_ok = false;
	      next ();
	      continue;
	    }
	  else if (t && t->type == tok_operator && t->content == "{")
	    {
	      locations.push_back(pp);
	      break;
	    }
	  else
            throw parse_error ("expected ',' or '{'");
          // XXX: unify logic with that in parse_symbol()
	}
      else
	throw parse_error ("expected probe point specifier");
    }

  if (aliases.empty())
    {
      probe* p = new probe;
      p->tok = t0;
      p->locations = locations;
      p->body = parse_stmt_block ();
      probe_ret.push_back (p);
    }
  else
    {
      probe_alias* p = new probe_alias (aliases);
      p->tok = t0;
      p->locations = locations;
      p->body = parse_stmt_block ();
      alias_ret.push_back (p);
    }
}


embeddedcode*
parser::parse_embeddedcode ()
{
  embeddedcode* e = new embeddedcode;
  const token* t = next ();
  if (t->type != tok_embedded)
    throw parse_error ("expected '%{'");

  if (! privileged)
    throw parse_error ("embedded code in unprivileged script");

  e->tok = t;
  e->code = t->content;
  return e;
}


block*
parser::parse_stmt_block ()
{
  block* pb = new block;

  const token* t = next ();
  if (! (t->type == tok_operator && t->content == "{"))
    throw parse_error ("expected '{'");

  pb->tok = t;

  while (1)
    {
      try
	{
	  t = peek ();
	  if (t && t->type == tok_operator && t->content == "}")
	    {
	      next ();
	      break;
	    }

          pb->statements.push_back (parse_statement ());
	}
      catch (parse_error& pe)
	{
	  print_error (pe);

	  // Quietly swallow all tokens until the next ';' or '}'.
	  while (1)
	    {
	      const token* t = peek ();
	      if (! t) return 0;
	      next ();
	      if (t->type == tok_operator
                  && (t->content == "}" || t->content == ";"))
		break;
	    }
	}
    }

  return pb;
}


statement*
parser::parse_statement ()
{
  const token* t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    {
      null_statement* n = new null_statement ();
      n->tok = next ();
      return n;
    }
  else if (t && t->type == tok_operator && t->content == "{")  
    return parse_stmt_block ();
  else if (t && t->type == tok_identifier && t->content == "if")
    return parse_if_statement ();
  else if (t && t->type == tok_identifier && t->content == "for")
    return parse_for_loop ();
  else if (t && t->type == tok_identifier && t->content == "foreach")
    return parse_foreach_loop ();
  else if (t && t->type == tok_identifier && t->content == "return")
    return parse_return_statement ();
  else if (t && t->type == tok_identifier && t->content == "delete")
    return parse_delete_statement ();
  else if (t && t->type == tok_identifier && t->content == "while")
    return parse_while_loop ();
  else if (t && t->type == tok_identifier && t->content == "break")
    return parse_break_statement ();
  else if (t && t->type == tok_identifier && t->content == "continue")
    return parse_continue_statement ();
  else if (t && t->type == tok_identifier && t->content == "next")
    return parse_next_statement ();
  // XXX: "do/while" statement?
  else if (t && (t->type == tok_operator || // expressions are flexible
                 t->type == tok_identifier ||
                 t->type == tok_number ||
                 t->type == tok_string))
    return parse_expr_statement ();
  // XXX: consider generally accepting tok_embedded here too
  else
    throw parse_error ("expected statement");
}


void
parser::parse_global (vector <vardecl*>& globals)
{
  const token* t0 = next ();
  if (! (t0->type == tok_identifier && t0->content == "global"))
    throw parse_error ("expected 'global'");

  while (1)
    {
      const token* t = next ();
      if (! (t->type == tok_identifier))
        throw parse_error ("expected identifier");

      for (unsigned i=0; i<globals.size(); i++)
	if (globals[i]->name == t->content)
          throw parse_error ("duplicate global name");

      vardecl* d = new vardecl;
      d->name = t->content;
      d->tok = t;
      globals.push_back (d);

      t = peek ();
      if (t && t->type == tok_operator && t->content == ",")
	{
	  next ();
	  continue;
	}
      else
	break;
    }
}


void
parser::parse_functiondecl (std::vector<functiondecl*>& functions)
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "function"))
    throw parse_error ("expected 'function'");


  t = next ();
  if (! (t->type == tok_identifier))
    throw parse_error ("expected identifier");

  for (unsigned i=0; i<functions.size(); i++)
    if (functions[i]->name == t->content)
      throw parse_error ("duplicate function name");

  functiondecl *fd = new functiondecl ();
  fd->name = t->content;
  fd->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw parse_error ("expected '('");

  while (1)
    {
      t = next ();

      // permit zero-argument fuctions
      if (t->type == tok_operator && t->content == ")")
        break;
      else if (! (t->type == tok_identifier))
	throw parse_error ("expected identifier");
      vardecl* vd = new vardecl;
      vd->name = t->content;
      vd->tok = t;
      fd->formal_args.push_back (vd);

      t = next ();
      if (t->type == tok_operator && t->content == ")")
	break;
      if (t->type == tok_operator && t->content == ",")
	continue;
      else
	throw parse_error ("expected ',' or ')'");
    }

  t = peek ();
  if (t && t->type == tok_embedded)
    fd->body = parse_embeddedcode ();
  else
    fd->body = parse_stmt_block ();

  functions.push_back (fd);
}


probe_point*
parser::parse_probe_point ()
{
  probe_point* pl = new probe_point;

  // XXX: add support for probe point aliases
  // e.g.   probe   alias = foo    { ... }
  while (1)
    {
      const token* t = next ();
      if (t->type != tok_identifier)
        throw parse_error ("expected identifier");

      if (pl->tok == 0) pl->tok = t;

      probe_point::component* c = new probe_point::component;
      c->functor = t->content;
      pl->components.push_back (c);
      // NB though we still may add c->arg soon

      t = peek ();
      if (t && t->type == tok_operator 
          && (t->content == "{" || t->content == "," || t->content == "="))
        break;
      
      if (t && t->type == tok_operator && t->content == "(")
        {
          next (); // consume "("
          c->arg = parse_literal ();

          t = next ();
          if (! (t->type == tok_operator && t->content == ")"))
            throw parse_error ("expected ')'");

          t = peek ();
          if (t && t->type == tok_operator 
              && (t->content == "{" || t->content == "," || t->content == "="))
            break;
          else if (t && t->type == tok_operator &&
                   t->content == "(")
            throw parse_error ("unexpected '.' or ',' or '{'");
        }
      // fall through

      if (t && t->type == tok_operator && t->content == ".")
        next ();
      else
        throw parse_error ("expected '.' or ',' or '(' or '{' or '='");
    }

  return pl;
}


literal*
parser::parse_literal ()
{
  const token* t = next ();
  literal* l;
  if (t->type == tok_string)
    l = new literal_string (t->content);
  else if (t->type == tok_number)
    {
      const char* startp = t->content.c_str ();
      char* endp = (char*) startp;

      // NB: we allow controlled overflow from LLONG_MIN .. ULLONG_MAX
      // Actually, this allows all the way from -ULLONG_MAX to ULLONG_MAX,
      // since the lexer only gives us positive digit strings.
      errno = 0;
      long long value = (long long) strtoull (startp, & endp, 0);
      if (errno == ERANGE || errno == EINVAL || *endp != '\0'
          || (unsigned long long) value > 18446744073709551615ULL
          || value < -9223372036854775807LL-1)
        throw parse_error ("number invalid or out of range"); 

      l = new literal_number (value);
    }
  else
    throw parse_error ("expected literal string or number");

  l->tok = t;
  return l;
}


if_statement*
parser::parse_if_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "if"))
    throw parse_error ("expected 'if'");
  if_statement* s = new if_statement;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw parse_error ("expected '('");

  s->condition = parse_expression ();

  t = next ();
  if (! (t->type == tok_operator && t->content == ")"))
    throw parse_error ("expected ')'");

  s->thenblock = parse_statement ();

  t = peek ();
  if (t && t->type == tok_identifier && t->content == "else")
    {
      next ();
      s->elseblock = parse_statement ();
    }

  return s;
}


expr_statement*
parser::parse_expr_statement ()
{
  expr_statement *es = new expr_statement;
  const token* t = peek ();
  es->tok = t;
  es->value = parse_expression ();
  return es;
}


return_statement*
parser::parse_return_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "return"))
    throw parse_error ("expected 'return'");
  return_statement* s = new return_statement;
  s->tok = t;
  s->value = parse_expression ();
  return s;
}


delete_statement*
parser::parse_delete_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "delete"))
    throw parse_error ("expected 'delete'");
  delete_statement* s = new delete_statement;
  s->tok = t;
  s->value = parse_expression ();
  return s;
}


next_statement*
parser::parse_next_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "next"))
    throw parse_error ("expected 'next'");
  next_statement* s = new next_statement;
  s->tok = t;
  return s;
}


break_statement*
parser::parse_break_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "break"))
    throw parse_error ("expected 'break'");
  break_statement* s = new break_statement;
  s->tok = t;
  return s;
}


continue_statement*
parser::parse_continue_statement ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "continue"))
    throw parse_error ("expected 'continue'");
  continue_statement* s = new continue_statement;
  s->tok = t;
  return s;
}


for_loop*
parser::parse_for_loop ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "for"))
    throw parse_error ("expected 'for'");
  for_loop* s = new for_loop;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw parse_error ("expected '('");

  // initializer + ";"
  t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    {
      literal_number* l = new literal_number(0);
      expr_statement* es = new expr_statement;
      es->value = l;
      s->init = es;
      es->value->tok = es->tok = next ();
    }
  else
    {
      s->init = parse_expr_statement ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ";"))
	throw parse_error ("expected ';'");
    }

  // condition + ";"
  t = peek ();
  if (t && t->type == tok_operator && t->content == ";")
    {
      literal_number* l = new literal_number(1);
      s->cond = l;
      s->cond->tok = next ();
    }
  else
    {
      s->cond = parse_expression ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ";"))
	throw parse_error ("expected ';'");
    }
  
  // increment + ")"
  t = peek ();
  if (t && t->type == tok_operator && t->content == ")")
    {
      literal_number* l = new literal_number(2);
      expr_statement* es = new expr_statement;
      es->value = l;
      s->incr = es;
      es->value->tok = es->tok = next ();
    }
  else
    {
      s->incr = parse_expr_statement ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ")"))
	throw parse_error ("expected ';'");
    }

  // block
  s->block = parse_statement ();

  return s;
}


for_loop*
parser::parse_while_loop ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "while"))
    throw parse_error ("expected 'while'");
  for_loop* s = new for_loop;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw parse_error ("expected '('");

  // dummy init and incr fields
  literal_number* l = new literal_number(0);
  expr_statement* es = new expr_statement;
  es->value = l;
  s->init = es;
  es->value->tok = es->tok = t;

  l = new literal_number(2);
  es = new expr_statement;
  es->value = l;
  s->incr = es;
  es->value->tok = es->tok = t;


  // condition
  s->cond = parse_expression ();


  t = next ();
  if (! (t->type == tok_operator && t->content == ")"))
    throw parse_error ("expected ')'");
  
  // block
  s->block = parse_statement ();

  return s;
}


foreach_loop*
parser::parse_foreach_loop ()
{
  const token* t = next ();
  if (! (t->type == tok_identifier && t->content == "foreach"))
    throw parse_error ("expected 'foreach'");
  foreach_loop* s = new foreach_loop;
  s->tok = t;

  t = next ();
  if (! (t->type == tok_operator && t->content == "("))
    throw parse_error ("expected '('");

  // see also parse_array_in

  bool parenthesized = false;
  t = peek ();
  if (t && t->type == tok_operator && t->content == "[")
    {
      next ();
      parenthesized = true;
    }

  while (1)
    {
      t = next ();
      if (! (t->type == tok_identifier))
        throw parse_error ("expected identifier");
      symbol* sym = new symbol;
      sym->tok = t;
      sym->name = t->content;
      s->indexes.push_back (sym);

      if (parenthesized)
        {
          const token* t = peek ();
          if (t && t->type == tok_operator && t->content == ",")
            {
              next ();
              continue;
            }
          else if (t && t->type == tok_operator && t->content == "]")
            {
              next ();
              break;
            }
          else 
            throw parse_error ("expected ',' or ']'");
        }
      else
        break; // expecting only one expression
    }

  t = next ();
  if (! (t->type == tok_identifier && t->content == "in"))
    throw parse_error ("expected 'in'");

  t = next ();
  if (t->type != tok_identifier)
    throw parse_error ("expected identifier");
  s->base = t->content;

  t = next ();
  if (! (t->type == tok_operator && t->content == ")"))
    throw parse_error ("expected ')'");

  s->block = parse_statement ();
  return s;
}


expression*
parser::parse_expression ()
{
  return parse_assignment ();
}


expression*
parser::parse_assignment ()
{
  expression* op1 = parse_ternary ();

  const token* t = peek ();
  // right-associative operators
  if (t && t->type == tok_operator 
      && (t->content == "=" ||
	  t->content == "<<<" ||
	  t->content == "+=" ||
	  t->content == "-=" ||
	  t->content == "*=" ||
	  t->content == "/=" ||
	  t->content == "%=" ||
	  t->content == "<<=" ||
	  t->content == ">>=" ||
	  t->content == "&=" ||
	  t->content == "^=" ||
	  t->content == "|=" ||
	  t->content == ".=" ||
	  false)) 
    {
      // NB: lvalueness is checked during elaboration / translation
      assignment* e = new assignment;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_expression ();
      op1 = e;
    }

  return op1;
}


expression*
parser::parse_ternary ()
{
  expression* op1 = parse_logical_or ();

  const token* t = peek ();
  if (t && t->type == tok_operator && t->content == "?")
    {
      ternary_expression* e = new ternary_expression;
      e->tok = t;
      e->cond = op1;
      next ();
      e->truevalue = parse_expression (); // XXX

      t = next ();
      if (! (t->type == tok_operator && t->content == ":"))
        throw parse_error ("expected ':'");

      e->falsevalue = parse_expression (); // XXX
      return e;
    }
  else
    return op1;
}


expression*
parser::parse_logical_or ()
{
  expression* op1 = parse_logical_and ();
  
  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "||")
    {
      logical_or_expr* e = new logical_or_expr;
      e->tok = t;
      e->op = t->content;
      e->left = op1;
      next ();
      e->right = parse_logical_and ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_logical_and ()
{
  expression* op1 = parse_boolean_or ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "&&")
    {
      logical_and_expr *e = new logical_and_expr;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_boolean_or ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_boolean_or ()
{
  expression* op1 = parse_boolean_xor ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "|")
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_boolean_xor ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_boolean_xor ()
{
  expression* op1 = parse_boolean_and ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "^")
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_boolean_and ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_boolean_and ()
{
  expression* op1 = parse_array_in ();

  const token* t = peek ();
  while (t && t->type == tok_operator && t->content == "&")
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_array_in ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_array_in ()
{
  // This is a very tricky case.  All these are legit expressions:
  // "a in b"  "a+0 in b" "[a,b] in c" "[c,(d+0)] in b"
  vector<expression*> indexes;
  bool parenthesized = false;

  const token* t = peek ();
  if (t && t->type == tok_operator && t->content == "[")
    {
      next ();
      parenthesized = true;
    }

  while (1)
    {
      expression* op1 = parse_comparison ();
      indexes.push_back (op1);

      if (parenthesized)
        {
          const token* t = peek ();
          if (t && t->type == tok_operator && t->content == ",")
            {
              next ();
              continue;
            }
          else if (t && t->type == tok_operator && t->content == "]")
            {
              next ();
              break;
            }
          else 
            throw parse_error ("expected ',' or ']'");
        }
      else
        break; // expecting only one expression
    }

  t = peek ();
  if (t && t->type == tok_identifier && t->content == "in")
    {
      array_in *e = new array_in;
      e->tok = t;
      next (); // swallow "in"

      arrayindex* a = new arrayindex;
      a->indexes = indexes;

      t = next ();
      if (t->type != tok_identifier)
        throw parse_error ("expected identifier");
      a->tok = t;
      a->base = t->content;

      e->operand = a;
      return e;
    }
  else if (indexes.size() == 1) // no "in" - need one expression only
    return indexes[0];
  else
    throw parse_error ("unexpected comma-separated expression list");
}


expression*
parser::parse_comparison ()
{
  expression* op1 = parse_shift ();

  const token* t = peek ();
  while (t && t->type == tok_operator 
      && (t->content == ">" ||
          t->content == "<" ||
          t->content == "==" ||
          t->content == "!=" ||
          t->content == "<=" ||
          t->content == ">="))
    {
      comparison* e = new comparison;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_shift ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_shift ()
{
  expression* op1 = parse_concatenation ();

  const token* t = peek ();
  while (t && t->type == tok_operator && 
         (t->content == "<<" || t->content == ">>"))
    {
      binary_expression* e = new binary_expression;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_concatenation ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_concatenation ()
{
  expression* op1 = parse_additive ();

  const token* t = peek ();
  // XXX: the actual awk string-concatenation operator is *whitespace*.
  // I don't know how to easily to model that here.
  while (t && t->type == tok_operator && t->content == ".")
    {
      concatenation* e = new concatenation;
      e->left = op1;
      e->op = t->content;
      e->tok = t;
      next ();
      e->right = parse_additive ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_additive ()
{
  expression* op1 = parse_multiplicative ();

  const token* t = peek ();
  while (t && t->type == tok_operator 
      && (t->content == "+" || t->content == "-"))
    {
      binary_expression* e = new binary_expression;
      e->op = t->content;
      e->left = op1;
      e->tok = t;
      next ();
      e->right = parse_multiplicative ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_multiplicative ()
{
  expression* op1 = parse_unary ();

  const token* t = peek ();
  while (t && t->type == tok_operator 
      && (t->content == "*" || t->content == "/" || t->content == "%"))
    {
      binary_expression* e = new binary_expression;
      e->op = t->content;
      e->left = op1;
      e->tok = t;
      next ();
      e->right = parse_unary ();
      op1 = e;
      t = peek ();
    }

  return op1;
}


expression*
parser::parse_unary ()
{
  const token* t = peek ();
  if (t && t->type == tok_operator 
      && (t->content == "+" || 
          t->content == "-" || 
          t->content == "!" ||
          t->content == "~" ||
          false))
    {
      unary_expression* e = new unary_expression;
      e->op = t->content;
      e->tok = t;
      next ();
      e->operand = parse_crement ();
      return e;
    }
  else
    return parse_crement ();
}


expression*
parser::parse_crement () // as in "increment" / "decrement"
{
  const token* t = peek ();
  if (t && t->type == tok_operator 
      && (t->content == "++" || t->content == "--"))
    {
      pre_crement* e = new pre_crement;
      e->op = t->content;
      e->tok = t;
      next ();
      e->operand = parse_value ();
      return e;
    }

  // post-crement or non-crement
  expression *op1 = parse_value ();
  
  t = peek ();
  if (t && t->type == tok_operator 
      && (t->content == "++" || t->content == "--"))
    {
      post_crement* e = new post_crement;
      e->op = t->content;
      e->tok = t;
      next ();
      e->operand = op1;
      return e;
    }
  else
    return op1;
}


expression*
parser::parse_value ()
{
  const token* t = peek ();
  if (! t)
    throw parse_error ("expected value");

  if (t->type == tok_operator && t->content == "(")
    {
      next ();
      expression* e = parse_expression ();
      t = next ();
      if (! (t->type == tok_operator && t->content == ")"))
        throw parse_error ("expected ')'");
      return e;
    }
  else if (t->type == tok_identifier)
    return parse_symbol ();
  else
    return parse_literal ();
}


// var, var[index], func(parms), thread->var, process->var
expression*
parser::parse_symbol () 
{
  string name;
  const token* t = expect_ident (name);
  const token* t2 = t;
  
  if (name.size() > 0 && name[0] == '$')
    {
      // target_symbol time
      target_symbol *tsym = new target_symbol;
      tsym->tok = t;
      tsym->base_name = name;
      while (true)
	{
	  string c;
	  if (peek_op ("."))
	    { 
	      next();
	      expect_ident (c);
	      tsym->components.push_back
		(make_pair (target_symbol::comp_struct_member, c));
	    }
	  else if (peek_op ("->"))
	    { 
	      next(); 
	      expect_ident (c);
	      tsym->components.push_back
		(make_pair (target_symbol::comp_struct_pointer_member, c));
	    }
	  else if (peek_op ("["))
	    { 
	      next();
	      expect_unknown (tok_number, c);
	      expect_op ("]");
	      tsym->components.push_back
		(make_pair (target_symbol::comp_literal_array_index, c));
	    }	    
	  else
	    break;
	}
      return tsym;
    }
  
  if (peek_op ("[")) // array
    {
      next ();
      struct arrayindex* ai = new arrayindex;
      ai->tok = t2;
      ai->base = name;
      while (1)
        {
          ai->indexes.push_back (parse_expression ());
          if (peek_op ("]"))
            { 
	      next(); 
	      break; 
	    }
          else if (peek_op (","))
	    {
	      next();
	      continue;
	    }
          else
            throw parse_error ("expected ',' or ']'");
        }
      return ai;
    }
  else if (peek_op ("(")) // function call
    {
      next ();
      struct functioncall* f = new functioncall;
      f->tok = t2;
      f->function = name;
      // Allow empty actual parameter list
      if (peek_op (")"))
	{
	  next ();
	  return f;
	}
      while (1)
	{
	  f->args.push_back (parse_expression ());
	  if (peek_op (")"))
	    {
	      next();
	      break;
	    }
	  else if (peek_op (","))
	    {
	      next();
	      continue;
	    }
	  else
	    throw parse_error ("expected ',' or ')'");
	}
      return f;
    }
  else
    {
      symbol* sym = new symbol;
      sym->name = name;
      sym->tok = t2;
      return sym;
    }
}

