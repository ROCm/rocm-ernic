#!/bin/bash
#
# Check if C source and header files conform to clang-format style
# (without modifying them)
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

echo "Checking C source and header file formatting..."
echo ""

# Find all .c and .h files
FILES=$(find src -type f \( -name "*.c" -o -name "*.h" \))
NEEDS_FORMAT=0
NEEDS_FORMAT_FILES=()

for file in $FILES; do
    if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
        echo "❌ $file needs formatting"
        NEEDS_FORMAT=1
        NEEDS_FORMAT_FILES+=("$file")
    fi
done

echo ""

if [ $NEEDS_FORMAT -eq 1 ]; then
    echo "❌ ${#NEEDS_FORMAT_FILES[@]} file(s) need formatting"
    echo ""
    echo "To fix, run:"
    echo "  ./scripts/format-code.sh"
    echo ""
    echo "Or manually:"
    echo "  clang-format -i ${NEEDS_FORMAT_FILES[*]}"
    exit 1
fi

echo "✅ All files are properly formatted!"

