# Changelog a718038


### Added

- Multimodal image indexing for `.bmp`, `.gif`, `.jpeg`, `.jpg`, `.png`, and `.webp`.
- [`src/image_processor.hpp`](src/image_processor.hpp) using [stb_image](src/third_party/stb_image.h) to extract a dominant color name per image.
- `color:` query (e.g. `color:red`) using the `file_record.dominant_color` value in the DB.

### Changed

- `file_record` now includes `dominant_color[16]`. Existing `idf_db/` directories must be removed and the workspace reindexed after upgrading.

### Architecture (producer–consumer indexing)

Indexing uses a reader–writer layout:

- **Readers:** HPX parallel workers in `fp::parse_loaded_batch` read files, tokenize or run the image processor, and enqueue `parse_batch` items.
- **Writer:** `idf::lmdb_writer` runs on a dedicated thread, drains a mutex-protected queue, and commits batches to LMDB in transactions.
