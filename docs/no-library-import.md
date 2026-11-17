# Editing an EPUB outside the library

`arianna --no-library-import /path/book.epub` opens a local EPUB without import. The toolbar marks it “Outside Library”. Edit Book, Edit at Text and Ctrl+E pass that exact file to the configured editor. Saves are explicit editor operations, and the existing file watcher reloads the reader. This mode does not create a library editor working copy or trigger BookTruthStore imports. Use a copy when the original must be preserved.

The mode is separate from `--open-read-only`. If both switches are present, read-only takes precedence and editing remains disabled. The new switch applies to EPUB files. Both cold startup and D-Bus command-line activation carry the mode. As usual, `--` ends option processing.

The reader's book entry, layout and position are temporary user/session data. Existing session-only BookServer registration serves immutable EPUB content and is removed when the reader session ends. Library metadata, annotation writes and structural commits require a library-managed book and remain unavailable in this mode. No new database tables, persistent derived data, or migration is introduced. File changes invalidate the existing BookServer container by version and file identity. Explicit Add to Library imports the current file through the established import path and clears the session-only mode.
