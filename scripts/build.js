#!/usr/bin/env node

import path from 'path';
import { createHash } from 'crypto';
import { mkdir, readFile, writeFile } from 'fs/promises';
import { spawn, exec } from 'child_process';
import { promisify } from 'util';
import { fileURLToPath } from 'url';
import { dirname } from 'path';
import { log, logSection, fileExists, findUEBuildTool, getProjectPaths } from './build-utils.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const execPromise = promisify(exec);

const atomicFingerprintInputs = [
  'Contracts/AtomicBlueprint/receipt-v1.schema.json',
  'Contracts/AtomicBlueprint/request-v1.schema.json',
  'Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp',
  'Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.h',
  'Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.cpp',
  'Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.h',
  'Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp',
  'Source/UE_MCP_Bridge/UE_MCP_Bridge.Build.cs',
];

async function generateAtomicBridgeBuildIdentity(projectRoot, gitCommit) {
  const pluginRoot = path.join(projectRoot, 'Plugins', 'UE_MCP_Bridge');
  const hash = createHash('sha256');
  for (const relativePath of [...atomicFingerprintInputs].sort()) {
    const relativeBytes = Buffer.from(relativePath, 'utf8');
    const sourceBytes = await readFile(path.join(pluginRoot, ...relativePath.split('/')));
    const relativeLength = Buffer.alloc(4);
    relativeLength.writeInt32LE(relativeBytes.length);
    const sourceLength = Buffer.alloc(4);
    sourceLength.writeInt32LE(sourceBytes.length);
    hash.update(relativeLength).update(relativeBytes).update(sourceLength).update(sourceBytes);
  }
  const fingerprint = hash.digest('hex');
  const generatedDirectory = path.join(pluginRoot, 'Intermediate', 'Generated', 'AtomicBridgeBuildIdentity');
  const generatedHeader = path.join(generatedDirectory, 'AtomicBridgeBuildIdentity.generated.h');
  const contents = `#pragma once\n\nnamespace UE_MCP_AtomicBuildIdentity\n{\n\tstatic constexpr const TCHAR* GitCommit = TEXT("${gitCommit}");\n\tstatic constexpr const TCHAR* BuildFingerprint = TEXT("${fingerprint}");\n\tstatic constexpr const TCHAR* PluginBuildIdentity = TEXT("sha256:${fingerprint}");\n}\n`;
  await mkdir(generatedDirectory, { recursive: true });
  let existing = '';
  try { existing = await readFile(generatedHeader, 'utf8'); } catch { /* generated on first build */ }
  if (existing !== contents) await writeFile(generatedHeader, contents, 'utf8');
  return fingerprint;
}

function runCommand(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const isWindows = process.platform === 'win32';
    
    if (isWindows && command.endsWith('.bat')) {
      // On Windows, use exec for batch files to properly handle paths with spaces
      // Quote the command path to handle spaces, then append args
      // Wrap the entire command+args in quotes for cmd /c
      const quotedCommand = `"${command}"`;
      const fullCommand = `cmd /c "${quotedCommand} ${args.join(' ')}"`;
      
      const proc = exec(fullCommand, {
        ...options,
      });

      // Pipe stdout and stderr to parent process
      if (proc.stdout) proc.stdout.pipe(process.stdout);
      if (proc.stderr) proc.stderr.pipe(process.stderr);

      proc.on('close', (code) => {
        if (code === 0) {
          resolve(code);
        } else {
          reject(new Error(`Command failed with exit code ${code}`));
        }
      });

      proc.on('error', (error) => {
        reject(error);
      });
    } else {
      // For non-batch files, use spawn
      const proc = spawn(command, args, {
        ...options,
        stdio: 'inherit',
        shell: false,
      });

      proc.on('close', (code) => {
        if (code === 0) {
          resolve(code);
        } else {
          reject(new Error(`Command failed with exit code ${code}`));
        }
      });

      proc.on('error', (error) => {
        reject(error);
      });
    }
  });
}

async function main() {
  logSection('UE-MCP Build');

  const { projectRoot, projectFile } = getProjectPaths();
  const repositoryRoot = path.resolve(__dirname, '..');

  // Check if project file exists
  if (!(await fileExists(projectFile))) {
    log(`ERROR: Project file not found at ${projectFile}`, 'red');
    process.exit(1);
  }

  // Find UE5 build tool
  const buildTool = findUEBuildTool();
  
  if (!buildTool) {
    log('ERROR: Unreal Engine build tool not found!', 'red');
    log('');
    log('Please either:');
    log('  1. Install UE5.3+ to default location, OR');
    log('  2. Set UE_BUILD_TOOL_PATH environment variable to your Build.bat path');
    log('');
    log('Example: set UE_BUILD_TOOL_PATH=C:\\Program Files\\Epic Games\\UE_5.8\\Engine\\Build\\BatchFiles\\Build.bat');
    process.exit(1);
  }

  log(`Project Root: ${projectRoot}`);
  log(`Project File: ${projectFile}`);
  log(`Build Tool: ${buildTool}`);
  log('');

  // Build command arguments
  const buildArgs = [
    'ue_mcpEditor',
    'Win64',
    'Development',
    `-Project="${projectFile}"`,
    '-WaitMutex',
    '-FromMsBuild',
    '-NoHotReloadFromIDE',
    '-NoUBTMakefiles',
  ];

  log('Starting build...');
  log(`Command: ${buildTool} ${buildArgs.join(' ')}`);
  log('');

  try {
    const { stdout: gitCommitOutput } = await execPromise('git rev-parse HEAD', { cwd: repositoryRoot });
    const gitCommit = gitCommitOutput.trim().toLowerCase();
    if (!/^[0-9a-f]{40}$/.test(gitCommit)) {
      throw new Error('Could not determine an exact bridge Git commit for build metadata');
    }
    const buildFingerprint = await generateAtomicBridgeBuildIdentity(projectRoot, gitCommit);
    log(`Bridge Git Commit: ${gitCommit}`);
    log(`Bridge Build Fingerprint: ${buildFingerprint}`);
    // Run the build
    await runCommand(buildTool, buildArgs, {
      env: { ...process.env, SPACEHEAD_BRIDGE_GIT_COMMIT: gitCommit },
    });
    
    logSection('Build succeeded!');
    process.exit(0);
  } catch (error) {
    logSection('Build failed!');
    log('Check the output above for errors.', 'red');
    process.exit(1);
  }
}

// Run the script
main().catch((error) => {
  log(`\nUnexpected error: ${error.message}`, 'red');
  process.exit(1);
});
