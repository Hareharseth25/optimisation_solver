// Minimal virtual nodes.
//
// Render functions return plain {tag, attrs, children} trees, so they are pure
// and testable in Node without a DOM. The browser turns a tree into elements
// with createElement/textContent only -- record text (model names, KAIRO
// messages) is never interpreted as HTML.

export function h(tag, attrs, ...children) {
  return { tag, attrs: attrs || {}, children: flatten(children) };
}

function flatten(list) {
  const out = [];
  for (const child of list) {
    if (child === null || child === undefined || child === false) continue;
    if (Array.isArray(child)) out.push(...flatten(child));
    else out.push(typeof child === 'object' ? child : String(child));
  }
  return out;
}

export function toDom(node, doc = globalThis.document) {
  if (typeof node === 'string') return doc.createTextNode(node);
  const element = doc.createElement(node.tag);
  for (const [name, value] of Object.entries(node.attrs)) {
    if (value === false || value === null || value === undefined) continue;
    element.setAttribute(name, value === true ? '' : String(value));
  }
  for (const child of node.children) element.appendChild(toDom(child, doc));
  return element;
}

// Visible text of a tree, whitespace-normalised (tests and accessibility checks).
export function textOf(node) {
  if (typeof node === 'string') return node;
  return node.children.map(textOf).join(' ').replace(/\s+/g, ' ').trim();
}

export function findAll(node, predicate, found = []) {
  if (typeof node === 'string') return found;
  if (predicate(node)) found.push(node);
  for (const child of node.children) findAll(child, predicate, found);
  return found;
}

export function byData(node, name, value) {
  return findAll(node, (n) => n.attrs[`data-${name}`] === value)[0];
}
