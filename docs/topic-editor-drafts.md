# Topic editor drafts and persisted metadata

The Book Details topic field is user input scoped to the open form. It remains a draft until the established save/finish-editing action writes through BookListModel. Progress and other metadata refreshes may update the displayed record but must not overwrite that draft. A successful save clears the draft flag only after reading and comparing the persisted topic hierarchy. A failed save retains the input and reports the failure.

The database is already the owner of library metadata. A read-only BookListModel accessor supplies current metadata when a details form opens with a stale delegate entry or receives another entry update. It performs no import, EPUB change, new analysis, or database migration. Existing EPUB subject persistence and hierarchy refinement remain unchanged. External/read-only books continue to use their supplied metadata and cannot acquire a library entry through this refresh.

The form snapshots BookEntry values into a plain JavaScript object because BookEntry equality represents file identity. Same-file metadata updates must still refresh the form. Draft text is applied explicitly to the FormTextFieldDelegate, which writes its own text property and can remove an external binding.
