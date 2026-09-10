'use strict';

const vscode = require('vscode');
const {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
  CloseAction,
  ErrorAction,
} = require('vscode-languageclient/node');
const path = require('path');

/** @type {LanguageClient | undefined} */
let client = undefined;

/** Resolve the lamo-lsp command: user setting first, bundled binary second. */
function serverCommand(context) {
  const cfg = vscode.workspace.getConfiguration('lamo');
  const configured = cfg.get('server.path', '').trim();
  if (configured) {
    return { command: configured, args: [] };
  }
  const bundled = context.asAbsolutePath(
    path.join('server', 'lamo-lsp' + (process.platform === 'win32' ? '.exe' : ''))
  );
  return { command: bundled, args: [] };
}

function activate(context) {
  const { command, args } = serverCommand(context);

  const serverOptions = {
    run: { command, args, transport: TransportKind.stdio },
    debug: { command, args, transport: TransportKind.stdio },
  };

  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'lamo' }],
    diagnosticCollectionName: 'lamo',
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.lamo'),
    },
    traceOutputChannel: vscode.window.createOutputChannel('Lamo Language Server (trace)'),
    outputChannel: vscode.window.createOutputChannel('Lamo Language Server'),
  };

  client = new LanguageClient('lamo', 'Lamo Language Server', serverOptions, clientOptions);

  // Virtual document provider: serves embedded std library sources so that
  // go-to-definition into `std.*` opens real code (scheme: lamo-std).
  const provider = vscode.languages.registerTextDocumentContentProvider(
    'lamo-std',
    {
      async provideTextDocumentContent(uri) {
        try {
          await client.onReady();
          const result = await client.sendRequest('lamo/stdContent', {
            uri: uri.toString(),
          });
          return typeof result === 'string' ? result : '';
        } catch (err) {
          return `// failed to load std source: ${err}`;
        }
      },
    }
  );
  context.subscriptions.push(provider);

  client.start();
  context.subscriptions.push({
    dispose: () => client && client.stop(),
  });
}

function deactivate() {
  if (client) {
    return client.stop();
  }
  return undefined;
}

module.exports = { activate, deactivate };
