/*
*   Copyright (c) 2000-2001, Thaddeus Covert <sahuagin@mediaone.net>
*   Copyright (c) 2002 Matthias Veit <matthias_veit@yahoo.de>
*   Copyright (c) 2004 Elliott Hughes <enh@acm.org>
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for generating tags for Ruby language
*   files.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <ctype.h>
#include <string.h>

#include "debug.h"
#include "entry.h"
#include "parse.h"
#include "nestlevel.h"
#include "read.h"
#include "routines.h"
#include "strlist.h"
#include "vstring.h"

/*
*   DATA DECLARATIONS
*/
typedef enum {
	K_UNDEFINED = -1, K_CLASS, K_METHOD, K_MODULE, K_SINGLETON, K_CONST,
} rubyKind;

/*
*   DATA DEFINITIONS
*/
static kindDefinition RubyKinds [] = {
	{ true, 'c', "class",  "classes" },
	{ true, 'f', "method", "methods" },
	{ true, 'm', "module", "modules" },
	{ true, 'S', "singletonMethod", "singleton methods" },
	{ true, 'C', "constant", "constants" },
};

typedef enum {
	F_MIXIN,
} rubyField;

static fieldDefinition RubyFields[] = {
	{ .name = "mixin",
	  .description = "how the class or module is mixed in (mixin:HOW:MODULE)",
	  .enabled = true },
};

struct blockData {
	stringList *mixin;
};

static NestingLevels* nesting = NULL;

#define SCOPE_SEPARATOR '.'

/*
*   FUNCTION DEFINITIONS
*/

static void enterUnnamedScope (void);

/*
* Returns a string describing the scope in 'nls'.
* We record the current scope as a list of entered scopes.
* Scopes corresponding to 'if' statements and the like are
* represented by empty strings. Scopes corresponding to
* modules and classes are represented by the name of the
* module or class.
*/
static vString* nestingLevelsToScope (const NestingLevels* nls)
{
	int i;
	unsigned int chunks_output = 0;
	vString* result = vStringNew ();
	for (i = 0; i < nls->n; ++i)
	{
	    NestingLevel *nl = nestingLevelsGetNth (nls, i);
	    tagEntryInfo *e = getEntryOfNestingLevel (nl);
	    if (e && strlen (e->name) > 0 && (!e->placeholder))
	    {
	        if (chunks_output++ > 0)
	            vStringPut (result, SCOPE_SEPARATOR);
	        vStringCatS (result, e->name);
	    }
	}
	return result;
}

/*
* Attempts to advance 's' past 'literal'.
* Returns true if it did, false (and leaves 's' where
* it was) otherwise.
*/
static bool canMatch (const unsigned char** s, const char* literal,
                         bool (*end_check) (int))
{
	const int literal_length = strlen (literal);
	const int s_length = strlen ((const char *)*s);

	if (s_length < literal_length)
		return false;

	const unsigned char next_char = *(*s + literal_length);
	if (strncmp ((const char*) *s, literal, literal_length) != 0)
	{
	    return false;
	}
	/* Additionally check that we're at the end of a token. */
	if (! end_check (next_char))
	{
	    return false;
	}
	*s += literal_length;
	return true;
}

static bool isIdentChar (int c)
{
	return (isalnum (c) || c == '_');
}

static bool notIdentChar (int c)
{
	return ! isIdentChar (c);
}

static bool isOperatorChar (int c)
{
	return (c == '[' || c == ']' ||
	        c == '=' || c == '!' || c == '~' ||
	        c == '+' || c == '-' ||
	        c == '@' || c == '*' || c == '/' || c == '%' ||
	        c == '<' || c == '>' ||
	        c == '&' || c == '^' || c == '|');
}

static bool notOperatorChar (int c)
{
	return ! isOperatorChar (c);
}

static bool isSigilChar (int c)
{
	return (c == '@' || c == '$');
}

static bool isWhitespace (int c)
{
	return c == 0 || isspace (c);
}

/*
 * Advance 's' while the passed predicate is true. Returns true if
 * advanced by at least one position.
 */
static bool advanceWhile (const unsigned char** s, bool (*predicate) (int))
{
	const unsigned char* original_pos = *s;

	while (**s != '\0')
	{
		if (! predicate (**s))
		{
			return *s != original_pos;
		}

		(*s)++;
	}

	return *s != original_pos;
}

static bool canMatchKeyword (const unsigned char** s, const char* literal)
{
	return canMatch (s, literal, notIdentChar);
}

/*
 * Extends canMatch. Works similarly, but allows assignment to precede
 * the keyword, as block assignment is a common Ruby idiom.
 */
static bool canMatchKeywordWithAssign (const unsigned char** s, const char* literal)
{
	const unsigned char* original_pos = *s;

	if (canMatchKeyword (s, literal))
	{
		return true;
	}

	advanceWhile (s, isSigilChar);

	if (! advanceWhile (s, isIdentChar))
	{
		*s = original_pos;
		return false;
	}

	advanceWhile (s, isWhitespace);

	if (! (advanceWhile (s, isOperatorChar) && *(*s - 1) == '='))
	{
		*s = original_pos;
		return false;
	}

	advanceWhile (s, isWhitespace);

	if (canMatchKeyword (s, literal))
	{
		return true;
	}

	*s = original_pos;
	return false;
}

/*
* Attempts to advance 'cp' past a Ruby operator method name. Returns
* true if successful (and copies the name into 'name'), false otherwise.
*/
static bool parseRubyOperator (vString* name, const unsigned char** cp)
{
	static const char* RUBY_OPERATORS[] = {
	    "[]", "[]=",
	    "**",
	    "!", "~", "+@", "-@",
	    "*", "/", "%",
	    "+", "-",
	    ">>", "<<",
	    "&",
	    "^", "|",
	    "<=", "<", ">", ">=",
	    "<=>", "==", "===", "!=", "=~", "!~",
	    "`",
	    NULL
	};
	int i;
	for (i = 0; RUBY_OPERATORS[i] != NULL; ++i)
	{
	    if (canMatch (cp, RUBY_OPERATORS[i], notOperatorChar))
	    {
	        vStringCatS (name, RUBY_OPERATORS[i]);
	        return true;
	    }
	}
	return false;
}

/*
* Emits a tag for the given 'name' of kind 'kind' at the current nesting.
*/
static int emitRubyTag (vString* name, rubyKind kind)
{
	tagEntryInfo tag;
	vString* scope;
	tagEntryInfo *parent;
	rubyKind parent_kind = K_UNDEFINED;
	NestingLevel *lvl;
	const char *unqualified_name;
	const char *qualified_name;
	int r;

        if (!RubyKinds[kind].enabled) {
            return CORK_NIL;
        }

	scope = nestingLevelsToScope (nesting);
	lvl = nestingLevelsGetCurrent (nesting);
	parent = getEntryOfNestingLevel (lvl);
	if (parent)
		parent_kind =  parent->kindIndex;

	qualified_name = vStringValue (name);
	unqualified_name = strrchr (qualified_name, SCOPE_SEPARATOR);
	if (unqualified_name && unqualified_name[1])
	{
		if (unqualified_name > qualified_name)
		{
			if (vStringLength (scope) > 0)
				vStringPut (scope, SCOPE_SEPARATOR);
			vStringNCatS (scope, qualified_name,
			              unqualified_name - qualified_name);
			/* assume module parent type for a lack of a better option */
			parent_kind = K_MODULE;
		}
		unqualified_name++;
	}
	else
		unqualified_name = qualified_name;

	initTagEntry (&tag, unqualified_name, kind);
	if (vStringLength (scope) > 0) {
		Assert (0 <= parent_kind &&
		        (size_t) parent_kind < (ARRAY_SIZE (RubyKinds)));

		tag.extensionFields.scopeKindIndex = parent_kind;
		tag.extensionFields.scopeName = vStringValue (scope);
	}
	r = makeTagEntry (&tag);

	if (kind != K_CONST)
		nestingLevelsPush (nesting, r);

	vStringClear (name);
	vStringDelete (scope);

	return r;
}

/* Tests whether 'ch' is a character in 'list'. */
static bool charIsIn (char ch, const char* list)
{
	return (strchr (list, ch) != NULL);
}

/* Advances 'cp' over leading whitespace. */
static void skipWhitespace (const unsigned char** cp)
{
	while (isspace (**cp))
	{
	    ++*cp;
	}
}

/*
* Copies the characters forming an identifier from *cp into
* name, leaving *cp pointing to the character after the identifier.
*/
static rubyKind parseIdentifier (
		const unsigned char** cp, vString* name, rubyKind kind)
{
	/* Method names are slightly different to class and variable names.
	 * A method name may optionally end with a question mark, exclamation
	 * point or equals sign. These are all part of the name.
	 * A method name may also contain a period if it's a singleton method.
	 */
	bool had_sep = false;
	const char* also_ok;
	if (kind == K_METHOD)
	{
		also_ok = ".?!=";
	}
	else if (kind == K_SINGLETON)
	{
		also_ok = "?!=";
	}
	else
	{
		also_ok = "";
	}

	skipWhitespace (cp);

	/* Check for an anonymous (singleton) class such as "class << HTTP". */
	if (kind == K_CLASS && **cp == '<' && *(*cp + 1) == '<')
	{
		return K_UNDEFINED;
	}

	/* Check for operators such as "def []=(key, val)". */
	if (kind == K_METHOD || kind == K_SINGLETON)
	{
		if (parseRubyOperator (name, cp))
		{
			return kind;
		}
	}

	/* Copy the identifier into 'name'. */
	while (**cp != 0 && (**cp == ':' || isIdentChar (**cp) || charIsIn (**cp, also_ok)))
	{
		char last_char = **cp;

		if (last_char == ':')
			had_sep = true;
		else
		{
			if (had_sep)
			{
				vStringPut (name, SCOPE_SEPARATOR);
				had_sep = false;
			}
			vStringPut (name, last_char);
		}
		++*cp;

		if (kind == K_METHOD)
		{
			/* Recognize singleton methods. */
			if (last_char == '.')
			{
				vStringClear (name);
				return parseIdentifier (cp, name, K_SINGLETON);
			}
		}

		if (kind == K_METHOD || kind == K_SINGLETON)
		{
			/* Recognize characters which mark the end of a method name. */
			if (charIsIn (last_char, "?!="))
			{
				break;
			}
		}
	}
	return kind;
}

static int readAndEmitTag (const unsigned char** cp, rubyKind expected_kind)
{
	int r = CORK_NIL;
	if (isspace (**cp))
	{
		vString *name = vStringNew ();
		rubyKind actual_kind = parseIdentifier (cp, name, expected_kind);

		if (actual_kind == K_UNDEFINED || vStringLength (name) == 0)
		{
			/*
			* What kind of tags should we create for code like this?
			*
			*    %w(self.clfloor clfloor).each do |name|
			*        module_eval <<-"end;"
			*            def #{name}(x, y=1)
			*                q, r = x.divmod(y)
			*                q = q.to_i
			*                return q, r
			*            end
			*        end;
			*    end
			*
			* Or this?
			*
			*    class << HTTP
			*
			* For now, we don't create any.
			*/
			enterUnnamedScope ();
		}
		else
		{
			r = emitRubyTag (name, actual_kind);
		}
		vStringDelete (name);
	}
	return r;
}

static void readAndStoreMixinSpec (const unsigned char** cp, const char *how_mixin)
{

	NestingLevel *nl = nestingLevelsGetCurrent (nesting);
	tagEntryInfo *e  = getEntryOfNestingLevel (nl);

	if (e->kindIndex == K_SINGLETON)
	{
		nl = nestingLevelsGetNth (nesting, nesting->n - 2);
		if (nl == NULL)
			return;
		e = getEntryOfNestingLevel (nl);
	}

	if (! (e->kindIndex == K_CLASS || e->kindIndex == K_MODULE))
		return;

	if (isspace (**cp))
	{
		vString *spec = vStringNewInit (how_mixin);
		vStringPut(spec, ':');

		size_t len = vStringLength (spec);
		parseIdentifier (cp, spec, K_MODULE);
		if (len == vStringLength (spec))
		{
			vStringDelete (spec);
			return;
		}

		struct blockData *bdata =  nestingLevelGetUserData (nl);
		if (bdata->mixin == NULL)
			bdata->mixin = stringListNew ();
		stringListAdd (bdata->mixin, spec);
	}
}

static void enterUnnamedScope (void)
{
	int r = CORK_NIL;
	NestingLevel *parent = nestingLevelsGetCurrent (nesting);
	tagEntryInfo *e_parent = getEntryOfNestingLevel (parent);

	if (e_parent)
	{
		tagEntryInfo e;
		initTagEntry (&e, "", e_parent->kindIndex);
		e.placeholder = 1;
		r = makeTagEntry (&e);
	}
	nestingLevelsPush (nesting, r);
}

static void attachMixinField (int corkIndex, stringList *mixinSpec)
{
	vString *mixinField = stringListItem (mixinSpec, 0);
	for (unsigned int i = 1; i < stringListCount (mixinSpec); i++)
	{
		vStringPut (mixinField, ',');
		vStringCat (mixinField, stringListItem (mixinSpec, 1));
	}

	attachParserFieldToCorkEntry (corkIndex, RubyFields [F_MIXIN].ftype,
								  vStringValue (mixinField));
}

static void deleteBlockData (NestingLevel *nl)
{
	struct blockData *bdata = nestingLevelGetUserData (nl);

	if (nl->corkIndex != CORK_NIL
		&& bdata->mixin != NULL
		&& stringListCount (bdata->mixin) > 0)
		attachMixinField (nl->corkIndex, bdata->mixin);

	if (bdata->mixin)
		stringListDelete (bdata->mixin);
}

static bool doesLineIncludeConstant (const unsigned char **cp, vString *constant)
{
	const unsigned char **p = cp;

	if (isspace (**p))
		skipWhitespace (p);

	if (isupper (**p))
	{
		while (**p != 0 && isIdentChar (**p))
		{
			vStringPut (constant, **p);
			++*p;
		}
		if (isspace (**p))
			skipWhitespace (p);

		if (**p == '=')
		{
			*cp = *p;
			return true;
		}
		vStringClear (constant);
	}

	return false;
}



static void findRubyTags (void)
{
	const unsigned char *line;
	bool inMultiLineComment = false;
	vString *constant = vStringNew ();

	nesting = nestingLevelsNewFull (sizeof (struct blockData), deleteBlockData);

	/* FIXME: this whole scheme is wrong, because Ruby isn't line-based.
	* You could perfectly well write:
	*
	*  def
	*  method
	*   puts("hello")
	*  end
	*
	* if you wished, and this function would fail to recognize anything.
	*/
	while ((line = readLineFromInputFile ()) != NULL)
	{
		const unsigned char *cp = line;
		/* if we expect a separator after a while, for, or until statement
		 * separators are "do", ";" or newline */
		bool expect_separator = false;

		if (canMatch (&cp, "=begin", isWhitespace))
		{
			inMultiLineComment = true;
			continue;
		}
		if (canMatch (&cp, "=end", isWhitespace))
		{
			inMultiLineComment = false;
			continue;
		}
		if (inMultiLineComment)
			continue;

		skipWhitespace (&cp);

		/* Avoid mistakenly starting a scope for modifiers such as
		*
		*   return if <exp>
		*
		* FIXME: we're fooled if someone does something heinous such as
		*
		*   puts("hello") \
		*       unless <exp>
		*/

		if (canMatchKeywordWithAssign (&cp, "for") ||
		    canMatchKeywordWithAssign (&cp, "until") ||
		    canMatchKeywordWithAssign (&cp, "while"))
		{
			expect_separator = true;
			enterUnnamedScope ();
		}
		else if (canMatchKeywordWithAssign (&cp, "case") ||
		         canMatchKeywordWithAssign (&cp, "if") ||
		         canMatchKeywordWithAssign (&cp, "unless"))
		{
			enterUnnamedScope ();
		}

		/*
		* "module M", "class C" and "def m" should only be at the beginning
		* of a line.
		*/
		if (canMatchKeywordWithAssign (&cp, "module"))
		{
			readAndEmitTag (&cp, K_MODULE);
		}
		else if (canMatchKeywordWithAssign (&cp, "class"))
		{
			int r = readAndEmitTag (&cp, K_CLASS);

			if (r != CORK_NIL)
			{
				skipWhitespace (&cp);
				if (*cp == '<' && *(cp + 1) != '<')
				{
					cp++;
					vString *parent = vStringNew ();
					parseIdentifier (&cp, parent, K_CLASS);
					if (vStringLength (parent) > 0)
					{
						tagEntryInfo *e = getEntryInCorkQueue (r);
						e->extensionFields.inheritance = vStringDeleteUnwrap (parent);
					}
					else
						vStringDelete (parent);
				}
			}
		}
		else if (canMatchKeywordWithAssign (&cp, "include"))
		{
			readAndStoreMixinSpec (&cp, "include");
		}
		else if (canMatchKeywordWithAssign (&cp, "prepend"))
		{
			readAndStoreMixinSpec (&cp, "prepend");
		}
		else if (canMatchKeywordWithAssign (&cp, "extend"))
		{
			readAndStoreMixinSpec (&cp, "extend");
		}
		else if (canMatchKeywordWithAssign (&cp, "def"))
		{
			rubyKind kind = K_METHOD;
			NestingLevel *nl = nestingLevelsGetCurrent (nesting);
			tagEntryInfo *e  = getEntryOfNestingLevel (nl);

			/* if the def is inside an unnamed scope at the class level, assume
			 * it's from a singleton from a construct like this:
			 *
			 * class C
			 *   class << self
			 *     def singleton
			 *       ...
			 *     end
			 *   end
			 * end
			 */
			if (e && e->kindIndex == K_CLASS && strlen (e->name) == 0)
				kind = K_SINGLETON;
			readAndEmitTag (&cp, kind);
		}
		else if (doesLineIncludeConstant (&cp, constant))
		{
			emitRubyTag (constant, K_CONST);
			vStringClear (constant);
		}

		while (*cp != '\0')
		{
			/* FIXME: we don't cope with here documents,
			* or regular expression literals, or ... you get the idea.
			* Hopefully, the restriction above that insists on seeing
			* definitions at the starts of lines should keep us out of
			* mischief.
			*/
			if (inMultiLineComment || isspace (*cp))
			{
				++cp;
			}
			else if (*cp == '#')
			{
				/* FIXME: this is wrong, but there *probably* won't be a
				* definition after an interpolated string (where # doesn't
				* mean 'comment').
				*/
				break;
			}
			else if (canMatchKeyword (&cp, "begin"))
			{
				enterUnnamedScope ();
			}
			else if (canMatchKeyword (&cp, "do"))
			{
				if (! expect_separator)
					enterUnnamedScope ();
				else
					expect_separator = false;
			}
			else if (canMatchKeyword (&cp, "end") && nesting->n > 0)
			{
				/* Leave the most recent scope. */
				nestingLevelsPop (nesting);
			}
			else if (*cp == '"')
			{
				/* Skip string literals.
				 * FIXME: should cope with escapes and interpolation.
				 */
				do {
					++cp;
				} while (*cp != 0 && *cp != '"');
				if (*cp == '"')
					cp++; /* skip the last found '"' */
			}
			else if (*cp == ';')
			{
				++cp;
				expect_separator = false;
			}
			else if (*cp != '\0')
			{
				do
					++cp;
				while (isIdentChar (*cp));
			}
		}
	}
	nestingLevelsFree (nesting);
	vStringDelete (constant);
}

extern parserDefinition* RubyParser (void)
{
	static const char *const extensions [] = { "rb", "ruby", NULL };
	parserDefinition* def = parserNew ("Ruby");
	def->kindTable      = RubyKinds;
	def->kindCount  = ARRAY_SIZE (RubyKinds);
	def->extensions = extensions;
	def->parser     = findRubyTags;
	def->fieldTable = RubyFields;
	def->fieldCount = ARRAY_SIZE (RubyFields);
	def->useCork    = true;
	return def;
}
