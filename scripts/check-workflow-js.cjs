const { spawnSync } = require('node:child_process');

const query = '[.jobs[].steps[]? | select((.uses // "") | startswith("actions/github-script@")) | {name: .name, script: .with.script}]';
let failed = false;

for (const file of process.argv.slice(2)) {
  const parsed = spawnSync('uvx', ['--from', 'yq==3.4.3', 'yq', '-c', query, file], { encoding: 'utf8' });
  if (parsed.status !== 0) {
    console.error(`Could not parse ${file}: ${parsed.error || parsed.stderr}`);
    failed = true;
    continue;
  }

  for (const step of JSON.parse(parsed.stdout)) {
    if (typeof step.script !== 'string') {
      console.error(`${file}: ${step.name || 'github-script'} has no inline script`);
      failed = true;
      continue;
    }
    // Expressions are parsed by Actionlint; use a valid identifier for ESLint.
    const code = step.script.replace(/\$\{\{[\s\S]*?\}\}/g, 'ACTION_EXPRESSION');
    const input = `void (async () => {\n${code}\n})();\n`;
    const lint = spawnSync('npx', [
      '--yes', 'eslint@10.14.0', '--config', 'scripts/eslint.config.cjs',
      '--stdin', '--stdin-filename', 'workflow.cjs',
    ], { encoding: 'utf8', input });
    if (lint.status !== 0) {
      console.error(`${file}: ${step.name || 'github-script'}\n${lint.error || lint.stdout || lint.stderr}`);
      failed = true;
    }
  }
}

process.exitCode = failed ? 1 : 0;
