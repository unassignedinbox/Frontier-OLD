#!/usr/bin/env node
/** Lightweight static integration check for the standalone Vehicle Lab preview. */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const folder = dirname(fileURLToPath(import.meta.url));
const read = (name) => readFileSync(join(folder, name), "utf8");
const html = read("index.html");
const script = read("vehicle-studio.js");
const style = read("vehicle-studio.css");

assert.match(html, /<link rel="stylesheet" href="vehicle-studio\.css">/);
assert.match(html, /<script src="vehicle-studio\.js"><\/script>/);
for (const label of ["Vehicle inspector", "Powertrain editor", "Tyre inspector", "Engine instrumentation"]) {
  assert.ok(script.includes(label), `missing ${label} panel`);
}
for (const selector of ["#vehicle-studio", ".vs-stage", ".vs-inspector", ".vs-telemetry-chart"]) {
  assert.ok(style.includes(selector), `missing ${selector} style`);
}

console.log("Vehicle Lab static integration: OK");
