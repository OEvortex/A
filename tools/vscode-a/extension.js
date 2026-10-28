// A Language extension: TextMate highlighting comes from the grammar in
// syntaxes/a.tmLanguage.json. Everything else is served by the a-lsp process
// over the Language Server Protocol. The client below is intentionally small
// and dependency-free so the extension can be loaded without npm install.

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

/** @type {LanguageClient | undefined} */
let client;
let outputChannel;
let restartWarned = false;

// --------------------------------------------------------------- client

class LanguageClient {
  constructor(serverPath, args, modulePaths) {
    this.serverPath = serverPath;
    this.args = args;
    this.modulePaths = modulePaths;
    this.process = undefined;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.ready = false;
    this.disposables = [];
    this.diagnostics = vscode.languages.createDiagnosticCollection("a");
    this.disposables.push(this.diagnostics);
  }

  async start() {
    const config = vscode.workspace.getConfiguration("a");
    const root = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length
      ? vscode.workspace.workspaceFolders[0].uri.toString()
      : null;

    this.process = cp.spawn(this.serverPath, this.args, {
      cwd: vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length
        ? vscode.workspace.workspaceFolders[0].uri.fsPath
        : undefined,
      stdio: ["pipe", "pipe", "pipe"],
      env: process.env,
      windowsHide: true,
    });

    this.process.stdout.on("data", (chunk) => this.onData(chunk));
    this.process.stderr.on("data", (chunk) => {
      const text = chunk.toString("utf8").trim();
      if (text) outputChannel.appendLine(text);
    });
    this.process.on("error", (error) => {
      outputChannel.appendLine(`A language server failed to start: ${error.message}`);
      for (const pending of this.pending.values()) {
        pending.reject(error);
      }
      this.pending.clear();
      if (this.onInitializeError) {
        const reject = this.onInitializeError;
        this.onInitializeError = undefined;
        reject(error);
      }
    });
    this.process.on("exit", (code, signal) => {
      this.ready = false;
      for (const pending of this.pending.values()) {
        pending.reject(new Error("The A language server exited."));
      }
      this.pending.clear();
      if (this.onInitializeError) {
        const reject = this.onInitializeError;
        this.onInitializeError = undefined;
        reject(new Error("The A language server exited during initialization."));
      }
      if (code !== 0 && code !== null) {
        outputChannel.appendLine(`A language server exited with code ${code}${signal ? ` (${signal})` : ""}.`);
      }
    });

    const initialized = new Promise((resolve, reject) => {
      this.onInitialized = resolve;
      this.onInitializeError = reject;
    });

    this.request("initialize", {
      processId: process.pid,
      clientInfo: { name: "a-vscode", version: "0.1.0" },
      rootUri: root,
      workspaceFolders: vscode.workspace.workspaceFolders
        ? vscode.workspace.workspaceFolders.map((folder) => ({
            uri: folder.uri.toString(),
            name: folder.name,
          }))
        : null,
      capabilities: {
        workspace: { workspaceFolders: true },
        textDocument: {
          synchronization: { didSave: true, dynamicRegistration: false },
          completion: { completionItem: { snippetSupport: false } },
          hover: { contentFormat: ["markdown", "plaintext"] },
          documentSymbol: { hierarchicalDocumentSymbolSupport: true },
          definition: { linkSupport: true },
          references: {},
          documentHighlight: {},
          formatting: {},
          rename: {},
          foldingRange: {},
        },
      },
      initializationOptions: {
        modulePaths: this.modulePaths,
      },
    }).catch((error) => {
      if (this.onInitializeError) this.onInitializeError(error);
    });

    await Promise.race([
      initialized,
      new Promise((_, reject) =>
        setTimeout(() => reject(new Error("Timed out waiting for a-lsp to initialize.")), 10000)
      ),
    ]);
    this.ready = true;
    this.notify("initialized", {});
  }

  stop() {
    this.ready = false;
    this.diagnostics.clear();
    for (const disposable of this.disposables) disposable.dispose();
    this.disposables = [];
    if (this.process) {
      try {
        this.notify("exit", {});
      } catch {
        // The pipe may already be closed.
      }
      try {
        this.process.kill();
      } catch {
        // Ignore.
      }
      this.process = undefined;
    }
  }

  send(message) {
    if (!this.process || !this.process.stdin.writable) return;
    const body = Buffer.from(JSON.stringify(message), "utf8");
    this.process.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.process.stdin.write(body);
  }

  notify(method, params) {
    this.send({ jsonrpc: "2.0", method, params });
  }

  request(method, params) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.send({ jsonrpc: "2.0", id, method, params });
    });
  }

  respond(id, result) {
    this.send({ jsonrpc: "2.0", id, result });
  }

  respondError(id, code, message) {
    this.send({ jsonrpc: "2.0", id, error: { code, message } });
  }

  onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (true) {
      const headerEnd = this.buffer.indexOf("\r\n\r\n");
      if (headerEnd < 0) return;
      const header = this.buffer.slice(0, headerEnd).toString("ascii");
      const match = /content-length:\s*(\d+)/i.exec(header);
      if (!match) {
        this.buffer = this.buffer.slice(headerEnd + 4);
        continue;
      }
      const length = Number.parseInt(match[1], 10);
      const start = headerEnd + 4;
      if (this.buffer.length < start + length) return;
      const body = this.buffer.slice(start, start + length).toString("utf8");
      this.buffer = this.buffer.slice(start + length);
      let message;
      try {
        message = JSON.parse(body);
      } catch (error) {
        outputChannel.appendLine(`Invalid message from a-lsp: ${error.message}`);
        continue;
      }
      this.handleMessage(message);
    }
  }

  handleMessage(message) {
    if (message.id !== undefined && !message.method) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) {
        pending.reject(new Error(message.error.message || "A language server error."));
      } else {
        const result = message.result;
        pending.resolve(result);
        if (this.onInitialized && result && result.capabilities) {
          const resolve = this.onInitialized;
          this.onInitialized = undefined;
          resolve();
        }
      }
      return;
    }

    if (message.method === "textDocument/publishDiagnostics") {
      this.publishDiagnostics(message.params);
      return;
    }

    if (message.id !== undefined) {
      if (message.method === "workspace/configuration") {
        this.respond(message.id, []);
      } else if (message.method === "client/registerCapability" || message.method === "window/workDoneProgress/create") {
        this.respond(message.id, null);
      } else {
        this.respondError(message.id, -32601, `unsupported request: ${message.method}`);
      }
    }
  }

  publishDiagnostics(params) {
    if (!params || !params.uri) return;
    const uri = vscode.Uri.parse(params.uri);
    const diagnostics = (params.diagnostics || []).map((item) => {
      const diagnostic = new vscode.Diagnostic(
        toRange(item.range),
        item.message || "",
        toSeverity(item.severity)
      );
      diagnostic.source = item.source || "a";
      if (item.code !== undefined) diagnostic.code = item.code;
      return diagnostic;
    });
    this.diagnostics.set(uri, diagnostics);
  }

  openDocument(document) {
    this.notify("textDocument/didOpen", {
      textDocument: {
        uri: document.uri.toString(),
        languageId: "a",
        version: document.version,
        text: document.getText(),
      },
    });
  }

  changeDocument(document) {
    this.notify("textDocument/didChange", {
      textDocument: { uri: document.uri.toString(), version: document.version },
      contentChanges: [{ text: document.getText() }],
    });
  }

  saveDocument(document) {
    this.notify("textDocument/didSave", {
      textDocument: { uri: document.uri.toString() },
      text: document.getText(),
    });
  }

  closeDocument(document) {
    this.notify("textDocument/didClose", {
      textDocument: { uri: document.uri.toString() },
    });
    this.diagnostics.delete(document.uri);
  }
}

// ------------------------------------------------------------- utilities

function toRange(range) {
  if (!range) return new vscode.Range(0, 0, 0, 0);
  return new vscode.Range(
    range.start.line,
    range.start.character,
    range.end.line,
    range.end.character
  );
}

function toSeverity(severity) {
  switch (severity) {
    case 1:
      return vscode.DiagnosticSeverity.Error;
    case 2:
      return vscode.DiagnosticSeverity.Warning;
    case 3:
      return vscode.DiagnosticSeverity.Information;
    case 4:
      return vscode.DiagnosticSeverity.Hint;
    default:
      return vscode.DiagnosticSeverity.Error;
  }
}

// LSP symbol and completion kinds are 1-based; the VS Code enums are 0-based.
function toSymbolKind(kind) {
  const value = typeof kind === "number" ? kind - 1 : 0;
  return Math.max(0, Math.min(25, value));
}

function toCompletionKind(kind) {
  const value = typeof kind === "number" ? kind - 1 : 0;
  return Math.max(0, Math.min(24, value));
}

function isADocument(document) {
  return document.languageId === "a" && document.uri.scheme === "file";
}

function expandPath(value) {
  if (!value) return "";
  let expanded = value;
  if (expanded.startsWith("~")) {
    expanded = path.join(os.homedir(), expanded.slice(1));
  }
  const folders = vscode.workspace.workspaceFolders || [];
  if (folders.length) {
    expanded = expanded.replace(/\$\{workspaceFolder\}/g, folders[0].uri.fsPath);
  }
  return expanded;
}

function executable(pathname) {
  try {
    fs.accessSync(pathname, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function whichOnPath(name) {
  const command = process.platform === "win32" ? "where" : "which";
  const result = cp.spawnSync(command, [name], { encoding: "utf8" });
  if (result.status !== 0 || !result.stdout) return "";
  return result.stdout.split(/\r?\n/).map((line) => line.trim()).find(Boolean) || "";
}

function buildCandidates(folder) {
  const candidates = [];
  const names = process.platform === "win32" ? ["a-lsp.exe", "a-lsp"] : ["a-lsp"];
  for (const name of names) {
    candidates.push(path.join(folder, "build", name));
  }
  try {
    for (const entry of fs.readdirSync(folder, { withFileTypes: true })) {
      if (entry.isDirectory() && entry.name.startsWith("build")) {
        for (const name of names) candidates.push(path.join(folder, entry.name, name));
      }
    }
  } catch {
    // Not a readable workspace directory.
  }
  return candidates;
}

function findServer(context) {
  const config = vscode.workspace.getConfiguration("a");
  const configured = expandPath(config.get("server.path", ""));
  if (configured) {
    return executable(configured) ? configured : "";
  }

  const environment = process.env.A_LSP_PATH;
  if (environment) {
    const expanded = expandPath(environment);
    if (executable(expanded)) return expanded;
  }

  for (const folder of vscode.workspace.workspaceFolders || []) {
    for (const candidate of buildCandidates(folder.uri.fsPath)) {
      if (executable(candidate)) return candidate;
    }
  }

  // Development checkout: <repo>/tools/vscode-a -> <repo>/build/a-lsp
  const repositoryRoot = path.resolve(context.extensionPath, "..", "..");
  for (const candidate of buildCandidates(repositoryRoot)) {
    if (executable(candidate)) return candidate;
  }

  const fromPath = whichOnPath("a-lsp");
  if (fromPath) return fromPath;
  return "";
}

function hoverContents(contents) {
  if (!contents) return new vscode.MarkdownString("");
  if (Array.isArray(contents)) {
    const markdown = new vscode.MarkdownString();
    for (const part of contents) {
      const value = typeof part === "string" ? part : part && part.value;
      if (value) markdown.appendMarkdown(value);
      markdown.appendMarkdown("\n\n");
    }
    return markdown;
  }
  if (typeof contents === "string") return new vscode.MarkdownString(contents);
  return new vscode.MarkdownString(contents.value || "");
}

function convertDocumentSymbol(symbol) {
  const converted = new vscode.DocumentSymbol(
    symbol.name,
    symbol.detail || "",
    toSymbolKind(symbol.kind),
    toRange(symbol.range),
    toRange(symbol.selectionRange)
  );
  if (Array.isArray(symbol.children) && symbol.children.length) {
    converted.children = symbol.children.map(convertDocumentSymbol);
  }
  return converted;
}

function toTextEdit(edit) {
  return new vscode.TextEdit(toRange(edit.range), edit.newText || "");
}

function workspaceEditFromLsp(edit) {
  const workspaceEdit = new vscode.WorkspaceEdit();
  if (!edit) return workspaceEdit;
  if (edit.changes) {
    for (const [uri, edits] of Object.entries(edit.changes)) {
      workspaceEdit.set(vscode.Uri.parse(uri), (edits || []).map(toTextEdit));
    }
  }
  if (Array.isArray(edit.documentChanges)) {
    for (const change of edit.documentChanges) {
      if (!change || !change.textDocument || !Array.isArray(change.edits)) continue;
      workspaceEdit.set(
        vscode.Uri.parse(change.textDocument.uri),
        change.edits.map(toTextEdit)
      );
    }
  }
  return workspaceEdit;
}

function locationsFrom(result) {
  if (!result) return null;
  const entries = Array.isArray(result) ? result : [result];
  const locations = [];
  for (const entry of entries) {
    if (!entry) continue;
    const uri = entry.uri || entry.targetUri;
    const range = entry.range || entry.targetSelectionRange || entry.targetRange;
    if (!uri || !range) continue;
    locations.push(new vscode.Location(vscode.Uri.parse(uri), toRange(range)));
  }
  return locations.length ? locations : null;
}

// --------------------------------------------------------------- providers

function registerProviders(context, getClient) {
  const selector = { language: "a", scheme: "file" };

  context.subscriptions.push(
    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(document, position) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/definition", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
        });
        return locationsFrom(result);
      },
    }),

    vscode.languages.registerReferencesProvider(selector, {
      async provideReferences(document, position, refContext) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/references", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
          context: { includeDeclaration: refContext.includeDeclaration },
        });
        return locationsFrom(result);
      },
    }),

    vscode.languages.registerHoverProvider(selector, {
      async provideHover(document, position) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/hover", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
        });
        if (!result || !result.contents) return null;
        const range = result.range ? toRange(result.range) : undefined;
        return new vscode.Hover(hoverContents(result.contents), range);
      },
    }),

    vscode.languages.registerCompletionItemProvider(
      selector,
      {
        async provideCompletionItems(document, position) {
          const active = getClient();
          if (!active) return null;
          const result = await active.request("textDocument/completion", {
            textDocument: { uri: document.uri.toString() },
            position: { line: position.line, character: position.character },
          });
          if (!result) return null;
          const entries = Array.isArray(result) ? result : result.items || [];
          const items = entries.map((entry) => {
            const item = new vscode.CompletionItem(entry.label, toCompletionKind(entry.kind));
            if (entry.detail) item.detail = entry.detail;
            if (entry.documentation) {
              item.documentation = new vscode.MarkdownString(
                typeof entry.documentation === "string" ? entry.documentation : entry.documentation.value
              );
            }
            item.insertText = typeof entry.insertText === "string" ? entry.insertText : entry.label;
            if (entry.filterText) item.filterText = entry.filterText;
            if (entry.sortText) item.sortText = entry.sortText;
            return item;
          });
          return new vscode.CompletionList(items, Boolean(result.isIncomplete));
        },
      },
      ".",
      "\""
    ),

    vscode.languages.registerDocumentSymbolProvider(selector, {
      async provideDocumentSymbols(document) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/documentSymbol", {
          textDocument: { uri: document.uri.toString() },
        });
        if (!Array.isArray(result)) return null;
        return result.map(convertDocumentSymbol);
      },
    }),

    vscode.languages.registerDocumentFormattingEditProvider(selector, {
      async provideDocumentFormattingEdits(document, options) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/formatting", {
          textDocument: { uri: document.uri.toString() },
          options: {
            tabSize: options.tabSize,
            insertSpaces: options.insertSpaces,
          },
        });
        if (!Array.isArray(result)) return null;
        return result.map(toTextEdit);
      },
    }),

    vscode.languages.registerRenameProvider(selector, {
      async provideRenameEdits(document, position, newName) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/rename", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
          newName,
        });
        if (!result) return null;
        return workspaceEditFromLsp(result);
      },
    }),

    vscode.languages.registerDocumentHighlightProvider(selector, {
      async provideDocumentHighlights(document, position) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/documentHighlight", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character },
        });
        if (!Array.isArray(result)) return null;
        return result.map((entry) => {
          const kind = Math.max(0, Math.min(2, (entry.kind || 1) - 1));
          return new vscode.DocumentHighlight(toRange(entry.range), kind);
        });
      },
    }),

    vscode.languages.registerFoldingRangeProvider(selector, {
      async provideFoldingRanges(document) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("textDocument/foldingRange", {
          textDocument: { uri: document.uri.toString() },
        });
        if (!Array.isArray(result)) return null;
        return result.map(
          (entry) =>
            new vscode.FoldingRange(
              entry.startLine,
              entry.endLine,
              entry.kind === "region"
                ? vscode.FoldingRangeKind.Region
                : entry.kind === "comment"
                  ? vscode.FoldingRangeKind.Comment
                  : undefined
            )
        );
      },
    }),

    vscode.languages.registerWorkspaceSymbolProvider({
      async provideWorkspaceSymbols(query) {
        const active = getClient();
        if (!active) return null;
        const result = await active.request("workspace/symbol", { query });
        if (!Array.isArray(result)) return null;
        return result
          .filter((entry) => entry && entry.location)
          .map(
            (entry) =>
              new vscode.SymbolInformation(
                entry.name,
                toSymbolKind(entry.kind),
                entry.containerName || "A",
                new vscode.Location(
                  vscode.Uri.parse(entry.location.uri),
                  toRange(entry.location.range)
                )
              )
          );
      },
    })
  );
}

// --------------------------------------------------------------- lifecycle

let openedDocuments = new Set();
const changeTimers = new Map();

function debounceChange(document, getClient) {
  const key = document.uri.toString();
  const existing = changeTimers.get(key);
  if (existing) clearTimeout(existing);
  changeTimers.set(
    key,
    setTimeout(() => {
      changeTimers.delete(key);
      const active = getClient();
      if (active && openedDocuments.has(key)) active.changeDocument(document);
    }, 250)
  );
}

async function startClient(context) {
  if (client) {
    client.stop();
    client = undefined;
  }
  openedDocuments = new Set();

  const serverPath = findServer(context);
  if (!serverPath) {
    if (!restartWarned) {
      restartWarned = true;
      const message =
        "The a-lsp language server was not found. Build it with 'cmake --build build --target a-lsp' " +
        "or set 'a.server.path' to the a-lsp executable.";
      outputChannel.appendLine(message);
      vscode.window.showWarningMessage(message);
    }
    return;
  }

  const config = vscode.workspace.getConfiguration("a");
  const args = Array.isArray(config.get("server.args", [])) ? config.get("server.args", []) : [];
  const modulePaths = Array.isArray(config.get("server.modulePaths", []))
    ? config.get("server.modulePaths", []).map(expandPath)
    : [];

  const started = new LanguageClient(serverPath, args, modulePaths);
  outputChannel.appendLine(`Starting A language server: ${serverPath}`);
  try {
    await started.start();
  } catch (error) {
    outputChannel.appendLine(`Could not start ${serverPath}: ${error.message}`);
    vscode.window.showErrorMessage(`A language server failed to start: ${error.message}`);
    started.stop();
    return;
  }

  client = started;
  for (const document of vscode.workspace.textDocuments) {
    if (!isADocument(document)) continue;
    openedDocuments.add(document.uri.toString());
    client.openDocument(document);
  }
}

function activate(context) {
  outputChannel = vscode.window.createOutputChannel("A Language Server");
  context.subscriptions.push(outputChannel);

  const getClient = () => client;
  registerProviders(context, getClient);

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((document) => {
      if (!client || !isADocument(document)) return;
      openedDocuments.add(document.uri.toString());
      client.openDocument(document);
    }),

    vscode.workspace.onDidChangeTextDocument((event) => {
      if (!client || !isADocument(event.document)) return;
      if (!openedDocuments.has(event.document.uri.toString())) return;
      if (!event.contentChanges.length) return;
      debounceChange(event.document, getClient);
    }),

    vscode.workspace.onDidSaveTextDocument((document) => {
      if (!client || !isADocument(document)) return;
      const key = document.uri.toString();
      const timer = changeTimers.get(key);
      if (timer) {
        clearTimeout(timer);
        changeTimers.delete(key);
      }
      if (openedDocuments.has(key)) client.saveDocument(document);
    }),

    vscode.workspace.onDidCloseTextDocument((document) => {
      if (!client || !isADocument(document)) return;
      const key = document.uri.toString();
      const timer = changeTimers.get(key);
      if (timer) clearTimeout(timer);
      changeTimers.delete(key);
      if (openedDocuments.delete(key)) client.closeDocument(document);
    }),

    vscode.workspace.onDidChangeConfiguration((event) => {
      if (!event.affectsConfiguration("a.server")) return;
      startClient(context);
    }),

    vscode.commands.registerCommand("a.restartServer", async () => {
      await startClient(context);
      vscode.window.showInformationMessage("A language server restarted.");
    }),

    {
      dispose() {
        for (const timer of changeTimers.values()) clearTimeout(timer);
        changeTimers.clear();
        if (client) client.stop();
        client = undefined;
      },
    }
  );

  return startClient(context);
}

function deactivate() {
  for (const timer of changeTimers.values()) clearTimeout(timer);
  changeTimers.clear();
  if (client) client.stop();
  client = undefined;
  if (outputChannel) outputChannel.dispose();
}

module.exports = { activate, deactivate };
