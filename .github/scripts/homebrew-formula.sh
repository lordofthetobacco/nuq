#!/usr/bin/env bash
# Prints the Homebrew formula for one nuq release.
#
#   homebrew-formula.sh VERSION SHA256SUMS [BASE_URL]
#
# SHA256SUMS lists the release packages as `sha256sum` prints them. BASE_URL
# is where they are downloaded from; it defaults to the GitHub release of
# VERSION. release.yml installs the formula from the packages it has just
# built (BASE_URL is then a file:// directory) and, when it publishes, writes
# it to lordofthetobacco/homebrew-tap as Formula/nuq.rb.
set -euo pipefail

[ $# -ge 2 ] || { echo "usage: $0 VERSION SHA256SUMS [BASE_URL]" >&2; exit 2; }
version=$1
sums=$2
repo=${GITHUB_REPOSITORY:-lordofthetobacco/nuq}
base=${3:-https://github.com/$repo/releases/download/v$version}

sha() {
    local file="nuq-$version-$1.tar.gz" hash
    hash=$(awk -v f="$file" '$2 == f || $2 == "*" f { print $1 }' "$sums")
    [[ $hash =~ ^[0-9a-f]{64}$ ]] || { echo "$sums has no checksum for $file" >&2; exit 1; }
    printf '%s' "$hash"
}

macos_arm64=$(sha macos-arm64)
linux_aarch64=$(sha linux-aarch64)
linux_x86_64=$(sha linux-x86_64)

# Homebrew reads the version from a release URL (.../download/v0.1.2/...).
# Any other base has to spell it out.
version_line=
[ $# -lt 3 ] || version_line=$'\n'"  version \"$version\""

cat <<EOF
# Written by the release workflow of $repo
# (.github/scripts/homebrew-formula.sh). The next release overwrites this
# file, so change it there.
class Nuq < Formula
  desc "Lightweight meta harness for Claude Code, Codex and Copilot agents"
  homepage "https://github.com/$repo"
  url "$base/nuq-$version-macos-arm64.tar.gz"$version_line
  sha256 "$macos_arm64"

  depends_on "tmux"

  on_macos do
    depends_on arch: :arm64
  end

  on_linux do
    on_arm do
      url "$base/nuq-$version-linux-aarch64.tar.gz"
      sha256 "$linux_aarch64"
    end
    on_intel do
      url "$base/nuq-$version-linux-x86_64.tar.gz"
      sha256 "$linux_x86_64"
    end
  end

  def install
    bin.install "bin/nuq"
    pkgshare.install "LICENSES"
  end

  test do
    assert_equal "nuq #{version}", shell_output("#{bin}/nuq version").strip
  end
end
EOF
