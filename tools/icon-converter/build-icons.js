#!/usr/bin/env node
//
// Manifest-driven icon build.
//
// Reads resources/icons.json, rasterizes each icon to resources/icons/<file>.png,
// and rewrites the `pebble.resources.media` block in package.json so the C side
// gets the matching RESOURCE_ID_IMAGE_* names. Work is incremental: an icon is
// only re-rendered when its source SVG, size or color changed (tracked by hash
// in .icons-cache.json). Outputs this tool previously made that are no longer in
// the manifest are pruned ("remove unused"); files it never made are left alone.
//
// A `from` value selects the source:
//   "local:<name>"  -> resources/svg-icons/<name>.svg   (your own SVGs)
//   "<name>"        -> node_modules/pixelarticons/svg/<name>.svg
//   "png:<file>"    -> resources/icons/<file>  (hand-made PNG, copied through as-is)
//
// Colors: pixelarticons / lucide SVGs paint with `fill="currentColor"`, which we
// substitute with the icon's `color` (default #000000). NOTE: icons drawn through
// the UI lib (list_item / footer / multitap_keyboard) are recolored to the row ink at
// runtime via tint_icon(), so `color` is a no-op for those — it only matters for
// non-tinted assets (the menu logo). Setting a non-black color on a `tinted` icon
// is therefore flagged as a warning.

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { Resvg } = require('@resvg/resvg-js');

const RENDER_VERSION = '1'; // bump to force a full re-render after a logic change

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const MANIFEST = path.join(REPO_ROOT, 'resources', 'icons.json');
const PKG = path.join(REPO_ROOT, 'package.json');
const RESOURCES_DIR = path.join(REPO_ROOT, 'resources');
const PIXEL_DIR = path.join(__dirname, 'node_modules', 'pixelarticons', 'svg');

// ---- SVG -> square SVG (ported verbatim from svg-to-png.js so output is byte-identical) ----

function parseViewBox(svg) {
  const match = svg.match(/\bviewBox\s*=\s*(["'])([^"']+)\1/i);
  if (!match) return null;
  const values = match[2].trim().split(/[\s,]+/).map(Number);
  if (values.length !== 4 || values.some((v) => !Number.isFinite(v))) {
    throw new Error(`Invalid viewBox: ${match[2]}`);
  }
  const [x, y, width, height] = values;
  return { x, y, width, height };
}

function getSvgParts(svg) {
  const match = svg.match(/<svg\b([^>]*)>([\s\S]*)<\/svg>\s*$/i);
  if (!match) throw new Error('Expected an SVG document with an <svg> root element.');
  return { attributes: match[1], body: match[2] };
}

function removeRootSizingAttributes(attributes) {
  return attributes.replace(/\s(?:width|height|viewBox)\s*=\s*(["']).*?\1/gi, '').trim();
}

function toSquareSvg(svg) {
  const viewBox = parseViewBox(svg);
  const { attributes, body } = getSvgParts(svg);
  const source = viewBox ?? (() => {
    const info = new Resvg(svg, { font: { loadSystemFonts: false } });
    return { x: 0, y: 0, width: info.width, height: info.height };
  })();

  const side = Math.max(source.width, source.height);
  const x = source.x - (side - source.width) / 2;
  const y = source.y - (side - source.height) / 2;
  const rootAttributes = removeRootSizingAttributes(attributes);
  const xmlns = /\bxmlns\s*=/.test(rootAttributes) ? '' : ' xmlns="http://www.w3.org/2000/svg"';
  const extra = rootAttributes ? ` ${rootAttributes}` : '';
  return `<svg${xmlns}${extra} viewBox="${x} ${y} ${side} ${side}">${body}</svg>`;
}

function rasterize(svgSource, size, color, fill) {
  const colored = svgSource.replace(/currentColor/g, color);
  let square = toSquareSvg(colored);
  // Inject a root-level fill so shapes without their own `fill` inherit it.
  // Shapes that set their own fill (e.g. pixelarticons' currentColor paths) win.
  if (fill) square = square.replace(/^<svg\b/, `<svg fill="${fill}"`);
  const resvg = new Resvg(square, {
    fitTo: { mode: 'width', value: size },
    font: { loadSystemFonts: false },
    shapeRendering: 1,
  });
  return Buffer.from(resvg.render().asPng());
}

// ---- helpers ----

const isBlack = (c) => /^#?(000|000000|black)$/i.test(String(c).replace(/\s/g, ''));
const sha = (buf) => crypto.createHash('sha256').update(buf).digest('hex').slice(0, 16);
const rel = (p) => path.relative(REPO_ROOT, p);

function parseFrom(from) {
  if (from.startsWith('local:')) return { kind: 'local', ref: from.slice(6) };
  if (from.startsWith('png:'))   return { kind: 'png',   ref: from.slice(4) };
  return { kind: 'pixel', ref: from };
}

function svgPathFor(kind, ref) {
  if (kind === 'local') return path.join(REPO_ROOT, manifest.localDir || 'resources/svg-icons', `${ref}.svg`);
  return path.join(PIXEL_DIR, `${ref}.svg`); // pixel
}

// Replace only the media array text so the rest of package.json keeps its
// hand-written formatting (targetPlatforms etc. stay on one line).
function writeMedia(media) {
  const text = fs.readFileSync(PKG, 'utf8');
  const re = /("media"\s*:\s*)\[[\s\S]*?\n\s*\]/;
  if (!re.test(text)) throw new Error('Could not locate the "media" array in package.json');
  const body = JSON.stringify(media, null, 2)
    .split('\n')
    .map((line, i) => (i === 0 ? line : '      ' + line))
    .join('\n');
  const next = text.replace(re, `$1${body}`);
  if (next !== text) fs.writeFileSync(PKG, next); // no-op when already up to date
}

// ---- build ----

const manifest = JSON.parse(fs.readFileSync(MANIFEST, 'utf8'));
const defaults = manifest.defaults || {};
const outputDir = path.join(REPO_ROOT, manifest.outputDir || 'resources/icons');
const cachePath = path.join(__dirname, '.icons-cache.json');
fs.mkdirSync(outputDir, { recursive: true });

let cache = {};
try { cache = JSON.parse(fs.readFileSync(cachePath, 'utf8')); } catch { /* first run */ }

const newCache = {};
const media = [];
const produced = new Set();
const warnings = [];
let made = 0, skipped = 0;

for (const icon of manifest.icons) {
  if (!icon.name) throw new Error(`Icon missing "name": ${JSON.stringify(icon)}`);
  const size = icon.size ?? defaults.size ?? 25;
  const color = icon.color ?? defaults.color ?? '#000000';
  const fill = icon.fill ?? defaults.fill ?? null;
  const tinted = icon.tinted ?? defaults.tinted ?? true;
  const { kind, ref } = parseFrom(icon.from);

  if (tinted && !isBlack(color)) {
    warnings.push(`${icon.name}: color ${color} is ignored — tinted icons are recolored to the row ink at runtime.`);
  }
  if (tinted && fill) {
    warnings.push(`${icon.name}: fill ${fill} is ignored — tinted icons are flattened to the row ink at runtime.`);
  }

  const outFile = kind === 'png' ? ref : (icon.file || `${path.basename(ref)}-${size}.png`);
  const outPath = path.join(outputDir, outFile);
  produced.add(outFile);

  if (kind === 'png') {
    if (!fs.existsSync(outPath)) {
      throw new Error(`${icon.name}: passthrough asset not found: ${rel(outPath)}`);
    }
    newCache[outFile] = sha(fs.readFileSync(outPath));
    skipped++;
  } else {
    const svgPath = svgPathFor(kind, ref);
    if (!fs.existsSync(svgPath)) {
      const where = kind === 'pixel' ? 'pixelarticons' : (manifest.localDir || 'resources/svg-icons');
      throw new Error(`${icon.name}: SVG "${ref}" not found in ${where} (${rel(svgPath)})`);
    }
    const svg = fs.readFileSync(svgPath, 'utf8');
    const key = sha(`${RENDER_VERSION}|${size}|${color}|${fill}|${svg}`);
    if (cache[outFile] === key && fs.existsSync(outPath)) {
      skipped++;
    } else {
      fs.writeFileSync(outPath, rasterize(svg, size, color, fill));
      made++;
      console.log(`  rendered  icons/${outFile}  (${size}px${isBlack(color) ? '' : ' ' + color})`);
    }
    newCache[outFile] = key;
  }

  media.push({
    type: 'bitmap',
    name: `IMAGE_${icon.name}`,
    file: path.relative(RESOURCES_DIR, outPath).split(path.sep).join('/'),
    ...(icon.menuIcon ? { menuIcon: true } : {}),
  });
}

// Prune outputs this tool previously made but the manifest dropped.
let pruned = 0;
for (const file of Object.keys(cache)) {
  if (produced.has(file)) continue;
  const p = path.join(outputDir, file);
  if (fs.existsSync(p)) { fs.unlinkSync(p); pruned++; console.log(`  pruned    icons/${file}`); }
}

// Note any files in the output dir we don't manage (left untouched).
for (const file of fs.readdirSync(outputDir)) {
  if (file.endsWith('.png') && !produced.has(file) && !(file in cache)) {
    warnings.push(`unmanaged file left in place: icons/${file} (not referenced by icons.json)`);
  }
}

writeMedia(media);
fs.writeFileSync(cachePath, JSON.stringify(newCache, null, 2) + '\n');

console.log(`\nicons: ${made} rendered, ${skipped} up-to-date, ${pruned} pruned, ${media.length} registered`);
for (const w of warnings) console.warn(`  ! ${w}`);
