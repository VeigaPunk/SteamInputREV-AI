pkgname=steaminput-harness
pkgver=0.1.0
pkgrel=1
pkgdesc="Drive Valve's Steam Input engine standalone via steamclient (SteamInputREV-AI)"
arch=(x86_64)
url="https://github.com/VeigaPunk/SteamInputREV-AI"
license=(MIT)
depends=(gcc-libs)
makedepends=(gcc make)
source=("https://github.com/VeigaPunk/SteamInputREV-AI/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
  cd "$pkgname-$pkgver" || cd "SteamInputREV-AI-$pkgver"
  make
}

package() {
  cd "$pkgname-$pkgver" || cd "SteamInputREV-AI-$pkgver"
  install -Dm755 steaminput-harness "$pkgdir/usr/bin/steaminput-harness"
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 manifests/dualsense_probe.vdf \
    "$pkgdir/usr/share/$pkgname/manifests/dualsense_probe.vdf"
}
