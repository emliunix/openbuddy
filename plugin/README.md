# OpenBuddy Plugin

Single-file TypeScript plugin for OpenCode. Loaded directly by OpenCode at runtime.

## Development

The `package.json` in this directory exists **only for type-checking** during development. It installs `@opencode-ai/sdk` and `@opencode-ai/plugin` as devDependencies so TypeScript can resolve the SDK event types.

OpenCode loads `openbuddy.ts` directly — no build step, no bundling.

### Type check

```sh
cd plugin
npm install
npx tsc --noEmit
```

### Install plugin

Project-local:
```sh
mkdir -p .opencode/plugins
cp plugin/openbuddy.ts .opencode/plugins/
```

Global:
```sh
mkdir -p ~/.config/opencode/plugins
cp plugin/openbuddy.ts ~/.config/opencode/plugins/
```
