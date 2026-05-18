# Conventions

## Languages & Tooling

| Component | Language | Minimum Standard | Build Tool |
|-----------|----------|------------------|------------|
| Daemon | Python | 3.12 | `uv` or `pip` |
| Buddy | C++ | C++20 | CMake 3.20+ |

## Naming

### Files
- `snake_case` everywhere.
- C++ headers: `.h` (plain C compatible) or `.hpp` (C++ only). Buddy uses `.h` for consistency with SDL examples.
- Python modules: `snake_case.py`.

### Code Identifiers

| Language | Type | Convention | Example |
|----------|------|------------|---------|
| Python | module | `snake_case` | `wire_protocol.py` |
| Python | class | `PascalCase` | `JsonRpcServer` |
| Python | function / variable | `snake_case` | `send_snapshot` |
| Python | constant | `UPPER_SNAKE_CASE` | `MAX_PACKET_SIZE` |
| C++ | file | `snake_case` | `tcp_client.cpp` |
| C++ | class / struct | `PascalCase` | `BuddyRenderer` |
| C++ | function / variable | `snake_case` | `draw_frame` |
| C++ | macro / constant | `UPPER_SNAKE_CASE` | `SCREEN_WIDTH` |

### JSON (Protocol)
- Keys: `snake_case`.
- Objects: keys sorted alphabetically when emitted (deterministic serialization).
- No trailing commas.

## Formatting

- **Line endings**: LF only. Enforced by `.gitattributes`:
  ```
  * text=auto eol=lf
  ```
- **Line width**: 100 columns (soft limit).
- **Indent**: 4 spaces (no tabs).
- **Braces**: K&R style — opening brace on the same line.

## Comments

- Prefer self-documenting names over comments.
- When a comment is necessary, explain *why*, not *what*.
- Use `TODO(username): description` for temporary notes.

## Git

- **Commits**: Conventional Commits (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`).
- **Branches**: `main` is protected. Use `feat/<name>` or `fix/<name>`.
- **PRs**: Require one review; CI must pass.

## Error Handling

- **Python**: Use exceptions for truly exceptional cases; return `Result`-like tuples for expected failures.
- **C++**: Use `std::optional` or `std::expected` (C++23 if available, else `tl::expected`) instead of output parameters. No bare `new`/`delete`.

## Dependencies

- **Daemon**: Only standard library + `asyncio` for networking. Add third-party libs only after discussion.
- **Buddy**: SDL2, SDL2_image, nlohmann/json (header-only). Vendor header-only deps in `buddy/vendor/`.
