#!/bin/bash
#
# Format all C source and header files using clang-format
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Check if clang-format is installed
if ! command -v clang-format &> /dev/null; then
    echo "❌ clang-format not found. Install with:"
    echo "   sudo apt-get install clang-format"
    exit 1
fi

echo "Formatting all C source and header files..."
echo ""

# Find and format all .c and .h files
FILES=$(find src -type f \( -name "*.c" -o -name "*.h" \))
COUNT=0

for file in $FILES; do
    clang-format -i "$file"
    echo "  ✓ $file"
    COUNT=$((COUNT + 1))
done

echo ""
echo "✅ Formatted $COUNT files"
echo ""
echo "Run 'git diff' to see changes"

