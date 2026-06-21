#!/usr/bin/env node

const fs = require('node:fs/promises');
const path = require('node:path');
const { Resvg } = require('@resvg/resvg-js');

const DEFAULT_SIZES = '50,25';
const DEFAULT_INPUT_DIR = path.resolve(__dirname, '..', '..', 'resources', 'svg-icons');

function parseSizes(value) {
  return value
    .split(',')
    .map((size) => Number.parseInt(size.trim(), 10))
    .filter((size) => Number.isInteger(size) && size > 0);
}

function parseViewBox(svg) {
  const match = svg.match(/\bviewBox\s*=\s*(["'])([^"']+)\1/i);
  if (!match) {
    return null;
  }

  const values = match[2].trim().split(/[\s,]+/).map(Number);
  if (values.length !== 4 || values.some((value) => !Number.isFinite(value))) {
    throw new Error(`Invalid viewBox: ${match[2]}`);
  }

  const [x, y, width, height] = values;
  return { x, y, width, height };
}

function getSvgParts(svg) {
  const match = svg.match(/<svg\b([^>]*)>([\s\S]*)<\/svg>\s*$/i);
  if (!match) {
    throw new Error('Expected an SVG document with an <svg> root element.');
  }

  return {
    attributes: match[1],
    body: match[2],
  };
}

function removeRootSizingAttributes(attributes) {
  return attributes
    .replace(/\s(?:width|height|viewBox)\s*=\s*(["']).*?\1/gi, '')
    .trim();
}

function toSquareSvg(svg) {
  const viewBox = parseViewBox(svg);
  const { attributes, body } = getSvgParts(svg);
  const renderInfo = new Resvg(svg, { font: { loadSystemFonts: false } });

  const source = viewBox ?? {
    x: 0,
    y: 0,
    width: renderInfo.width,
    height: renderInfo.height,
  };

  const side = Math.max(source.width, source.height);
  const x = source.x - (side - source.width) / 2;
  const y = source.y - (side - source.height) / 2;
  const rootAttributes = removeRootSizingAttributes(attributes);
  const xmlns = /\bxmlns\s*=/.test(rootAttributes) ? '' : ' xmlns="http://www.w3.org/2000/svg"';
  const extraAttributes = rootAttributes ? ` ${rootAttributes}` : '';

  return `<svg${xmlns}${extraAttributes} viewBox="${x} ${y} ${side} ${side}">${body}</svg>`;
}

async function convertSvg(svgPath, outputDir, sizes) {
  const source = await fs.readFile(svgPath, 'utf8');
  const squareSvg = toSquareSvg(source);
  const name = path.basename(svgPath, path.extname(svgPath));

  for (const size of sizes) {
    const resvg = new Resvg(squareSvg, {
      fitTo: { mode: 'width', value: size },
      font: { loadSystemFonts: false },
      shapeRendering: 1,
    });
    const png = resvg.render();
    const outputPath = path.join(outputDir, `${name}-${size}.png`);

    await fs.writeFile(outputPath, png.asPng());
    console.log(`${path.relative(process.cwd(), outputPath)} (${png.width}x${png.height})`);
  }
}

async function main() {
  const inputDir = path.resolve(process.argv[2] ?? DEFAULT_INPUT_DIR);
  const outputDir = path.resolve(process.argv[3] ?? path.join(inputDir, 'out'));
  const sizes = parseSizes(process.argv[4] ?? DEFAULT_SIZES);

  if (sizes.length === 0) {
    throw new Error('Provide one or more PNG sizes, for example: 50,25');
  }

  await fs.mkdir(outputDir, { recursive: true });

  const entries = await fs.readdir(inputDir, { withFileTypes: true });
  const svgFiles = entries
    .filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.svg'))
    .map((entry) => path.join(inputDir, entry.name))
    .sort();

  if (svgFiles.length === 0) {
    throw new Error(`No SVG files found in ${inputDir}`);
  }

  for (const svgFile of svgFiles) {
    await convertSvg(svgFile, outputDir, sizes);
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
