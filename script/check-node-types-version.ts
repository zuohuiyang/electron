import { promises as fs } from 'fs';
import * as path from 'path';

async function readDepsNodeVersion (depsPath: string): Promise<string> {
  const depsContents = await fs.readFile(depsPath, { encoding: 'utf8' });

  // TODO: this regex isn't good enough and technically this requires a real
  // parser to do correctly...... but this is good enough until then.
  const nodeVersion = depsContents.match(/'node_version':\s+'v([^']+)'/);

  if (nodeVersion === null || nodeVersion.length < 2) {
    throw new Error('Could not find node version in DEPS file');
  }

  return nodeVersion[1];
}

async function readPackageJsonVersion (packagePath: string, packageDepsKey: string): Promise<string> {
  const pkg = JSON.parse(await fs.readFile(packagePath, { encoding: 'utf8' }));

  if (!(typeof pkg === 'object') || !(packageDepsKey in pkg) || !('@types/node' in pkg[packageDepsKey]) || typeof pkg[packageDepsKey]['@types/node'] !== 'string') {
    throw new Error(`Could not find @types/node version for package "${packagePath}"`);
  }

  // TODO: this isn't a proper semver parse of the types, but the version in
  // DEPS likely will not match if this isn't semver either
  const nodeVersion = pkg[packageDepsKey]['@types/node'].match(/[\d.]+$/);

  if (nodeVersion === null) {
    throw new Error(`Could not find @types/node version for package "${packagePath}"`);
  }

  return nodeVersion[0];
}

const rootDir = path.resolve(__dirname, '..');
const depsPath = path.resolve(rootDir, 'DEPS');
const packagePaths = [
  // Repo dev package.json
  [path.resolve(rootDir, 'package.json'), 'devDependencies'],
  // Published package package.json
  [path.resolve(rootDir, 'npm', 'package.json'), 'dependencies']
];

(async () => {
  // Read the version of node from DEPS as source of truth
  const nodeVersion = await readDepsNodeVersion(depsPath);

  // Check each package in parallel
  await Promise.all(packagePaths.map(async ([packagePath, packageDepsKey]) => {
    const packageVersion = await readPackageJsonVersion(packagePath, packageDepsKey);
    if (packageVersion !== nodeVersion) {
      console.error(`\`@types/node\` version in "${packagePath}" (${packageVersion}) does not match deps version (${nodeVersion})`);
      process.exitCode = 1;
    }
  }));
})().catch(err => {
  console.error(err);
  process.exitCode = 1;
});
