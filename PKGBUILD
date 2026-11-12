# Maintainer: janderssn
pkgname=omarchy-presence
pkgver=0.1.0
pkgrel=1
pkgdesc="Presence sensing service: says on D-Bus whether someone is at the laptop"
arch=('x86_64' 'aarch64')
url="https://github.com/janderssn/omarchy-presence"
license=('MIT')
depends=('systemd-libs')
makedepends=('gcc' 'make' 'pkgconf' 'linux-api-headers')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
  cd "$pkgname-$pkgver"
  make
}

check() {
  cd "$pkgname-$pkgver"
  make check
}

package() {
  cd "$pkgname-$pkgver"
  make DESTDIR="$pkgdir" PREFIX=/usr install
}
