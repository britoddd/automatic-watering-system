#!/bin/bash
# Copy of ~/deploy-garden.sh on the VPS (run by .github/workflows/deploy.yml as user "deploy").
set -euo pipefail
cd ~/garden-web
git fetch --prune origin
git reset --hard origin/main
cd webapp
npm ci
npm run build        # runs the self-test; a failure stops the deploy before the restart
sudo systemctl restart garden-web
sleep 3
systemctl is-active --quiet garden-web && echo "Deploy OK"
