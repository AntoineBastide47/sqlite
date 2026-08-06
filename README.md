# MatcherText additions to the SQLite driver

This SQLite tree adds an opt-in MatcherText input discipline. The additions
check the balance of `()`, `[]`, and `{}` before an external value can change
an SQL boundary. All other bytes are nonmatchers.

The additions do not change the SQLite database file format.

## Build modes

Build the additive mode with `SQLITE_ENABLE_MATCHERTEXT`:

```sh
./configure --dev
make clean
make OPTIONS="-DSQLITE_ENABLE_MATCHERTEXT" sqlite3 sqlite3.c
```

This mode adds the MatcherText syntax and APIs. The standard SQLite prepare
and execution APIs remain available.

Add `SQLITE_MATCHERTEXT_STRICT` to require the checked preparation API for
external SQL:

```sh
make clean
make OPTIONS="-DSQLITE_ENABLE_MATCHERTEXT -DSQLITE_MATCHERTEXT_STRICT" \
  sqlite3 sqlite3.c testfixture
```

A build without `SQLITE_ENABLE_MATCHERTEXT` has the standard SQLite behavior.

## MatcherText rule

A value is MatcherText when its ASCII matcher pairs are correctly nested:

```text
valid:   text
valid:   call(item[index])
invalid: text]
invalid: ([)]
```

The scanner is byte-based and linear. UTF-8 continuation bytes cannot be
ASCII matchers. The default maximum nesting depth is 1,000. Set
`SQLITE_MAX_MATCHER_DEPTH` at compile time to use another limit.

SQL template checks do not count matcher bytes inside SQL comments. Matcher
bytes in literals, identifiers, templates, and input arguments remain checked.

## Checked preparation API

`sqlite3_matchertext_prepare_v3()` composes one checked SQL template. It adds
two typed placeholders:

- `?V` is a text value. SQLite verifies it and binds it as a parameter.
- `?I` is an identifier. SQLite verifies it and adds identifier delimiters.

Example:

```c
const char *name = "alice";
sqlite3_matchertext_arg args[2] = {
  {SQLITE_MATCHERTEXT_IDENTIFIER, "users", 5},
  {SQLITE_MATCHERTEXT_VALUE, name, (sqlite3_uint64)strlen(name)}
};
sqlite3_stmt *stmt = 0;
int rc = sqlite3_matchertext_prepare_v3(
  db,
  "SELECT * FROM ?I WHERE name=?V",
  -1,
  0,
  args,
  2,
  &stmt
);
```

The API enforces these rules before SQLite parses the composed SQL:

- The template must be NUL-free and valid MatcherText.
- Each placeholder must have one argument of the correct type.
- Every argument must be valid MatcherText.
- An identifier must be non-NULL and NUL-free.
- The template must contain one SQL statement.

The output statement uses the standard `sqlite3_step()`, `sqlite3_reset()`,
and `sqlite3_finalize()` lifecycle. The `prepFlags` argument accepts the normal
`sqlite3_prepare_v3()` flags.

## Value and identifier holes

The tokenizer adds a value literal whose boundary is found by matcher balance:

```sql
SELECT M'(Alice (admin))';
```

The value of this expression is `Alice (admin)`. Quotes and SQL text inside
the hole remain data because they do not define its end.

Square-bracket identifiers also use matcher balance:

```sql
SELECT [display(name)] FROM [users];
```

The outer `[]` delimit the identifier. Matcher pairs inside the name must be
balanced.

## Verify-or-refuse and encode-and-embed

`?V` is the verify-or-refuse path. It preserves the value bytes and rejects a
value with unmatched matchers.

The new `%m` conversion in `sqlite3_mprintf()` is the encode-and-embed path.
It accepts arbitrary NUL-terminated text, escapes only unmatched matchers and
backslashes, and writes an `M'(...)'` value literal. The parser decodes the
literal to the original value.

```c
char *sql = sqlite3_mprintf("SELECT %m", "smile :]");
/* sql is: SELECT M'(smile :\c[])' */

sqlite3_stmt *stmt = 0;
int rc = sqlite3_matchertext_prepare_v3(db, sql, -1, 0, 0, 0, &stmt);
sqlite3_free(sql);
```

Balanced values without backslashes keep their original spelling inside the
literal. The encoder changes only the representation required to preserve the
hole boundary. The SQL value after decoding is unchanged.

## Public verification and encoding APIs

The build adds these C interfaces:

```c
int sqlite3_matchertext_verify(
  const char *value,
  sqlite3_int64 length
);

char *sqlite3_matchertext_encode(
  const char *value,
  sqlite3_int64 length,
  sqlite3_int64 *output_length
);

char *sqlite3_matchertext_decode(
  const char *value,
  sqlite3_int64 length,
  sqlite3_int64 *output_length
);
```

A negative input length means that the input is NUL-terminated. The encoder
and decoder return memory from `sqlite3_malloc64()`. Release it with
`sqlite3_free()`.

The escape alphabet is:

| Input | Encoded form |
| --- | --- |
| `\` | `\\` |
| `(` | `\o()` |
| `)` | `\c()` |
| `[` | `\o[]` |
| `]` | `\c[]` |
| `{` | `\o{}` |
| `}` | `\c{}` |

Only unmatched matcher characters use these forms. A matched pair remains
unchanged.

## Strict mode

`SQLITE_MATCHERTEXT_STRICT` refuses external SQL passed through the legacy
UTF-8 and UTF-16 `sqlite3_prepare*()` APIs and through `sqlite3_exec()`. The
error directs the caller to `sqlite3_matchertext_prepare_v3()`.

SQLite can still prepare its own internal SQL for schema work, `ANALYZE`,
`VACUUM`, attached databases, and statement recompilation. Strict mode has no
run-time PRAGMA or database-configuration switch.

## Implementation files

The principal additions are:

- `src/matchertext.c`: verification, boundary scan, encoder, and decoder
- `src/tokenize.c` and `src/parse.y`: value and identifier hole syntax
- `src/prepare.c`: typed checked preparation
- `src/printf.c`: the `%m` conversion
- `src/sqlite.h.in`: public API declarations
- `src/complete.c`: complete-statement scanning for MatcherText literals
- `src/legacy.c`: strict-mode entry-point guard
- `test/matchertext.test`: scanner, parser, API, strict-mode, and regression tests

## Tests

Build the strict test fixture and run the dedicated suite:

```sh
make clean
make OPTIONS="-DSQLITE_ENABLE_MATCHERTEXT -DSQLITE_MATCHERTEXT_STRICT" \
  testfixture
./testfixture test/matchertext.test
```

Compile the SQLite library or amalgamation with `SQLITE_ENABLE_MATCHERTEXT`.
Client code that uses the new declarations must also define this flag when it
includes `sqlite3.h`. Only the library build needs
`SQLITE_MATCHERTEXT_STRICT`.
