#!/bin/sh
# Copies the shared figure runtime (DOM helpers, the floating contents column) and the shared page styles from the blog,
# where they are maintained, into docs/assets/explainer/. Edit them in the blog repo, then run this.
#   BLOG=~/Projects/blog scripts/sync-explainer-core.sh
set -eu
BLOG="${BLOG:-$HOME/Projects/blog}"
DEST="$(dirname "$0")/../docs/assets/explainer"
mkdir -p "$DEST/core"
for f in dom.js toc.js; do cp "$BLOG/src/explainer/core/$f" "$DEST/core/$f"; done
cp "$BLOG/src/styles/explainer.css" "$DEST/core.css"
echo "synced from $BLOG"
