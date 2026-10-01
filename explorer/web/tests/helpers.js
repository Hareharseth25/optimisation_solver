import { readFileSync } from 'node:fs';
import { textOf, findAll, byData } from '../js/vdom.js';
import { renderResult } from '../js/report.js';

// A response captured from the real Explorer service (fixtures/make_fixtures.py).
export function fixture(name) {
  const { httpStatus, body } = JSON.parse(readFileSync(new URL(`./fixtures/${name}.json`, import.meta.url)));
  return { kind: 'response', httpStatus, body };
}

export function render(name, context = { fileName: 'model.mps', fileSize: 100 }) {
  const result = fixture(name);
  return { result, record: result.body.record, tree: renderResult(result, context) };
}

export const section = (tree, name) => byData(tree, 'section', name);
export const text = (node) => (node ? textOf(node) : '');
export const field = (tree, name) => text(byData(tree, 'field', name));
export const role = (tree, name) => text(byData(tree, 'role', name));
export const notRuns = (node) => findAll(node, (n) => n.attrs['data-state'] === 'not-run');
