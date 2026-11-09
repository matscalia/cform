// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matteo Scalia

'use strict';

// Run with: node wasm/example.js
//
// The prompt UI is drawn on stderr; the resulting JSON is printed on stdout.
const { loadCform } = require('./index.js');

(async () => {
  const cform = await loadCform();

  const schema = {
    type: 'object',
    title: 'service',
    properties: {
      name: { type: 'string', description: 'service name' },
      port: { type: 'number', default: 8080 },
      tags: { type: 'array', items: { type: 'string' } },
    },
    required: ['name'],
  };

  // Optional starting document:
  // const document = { name: 'api', port: 8080 };
  const value = cform.run(schema);

  process.stdout.write(JSON.stringify(value, null, 2) + '\n');
})().catch(error => {
  process.stderr.write(`cform: ${error.message}\n`);
  process.exit(1);
});
