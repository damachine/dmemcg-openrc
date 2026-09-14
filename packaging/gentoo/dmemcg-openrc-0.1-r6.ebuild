EAPI=8

inherit toolchain-funcs

DESCRIPTION="DMEM cgroup broker for games on OpenRC"
HOMEPAGE="https://github.com/damachine/dmemcg-openrc"
SRC_URI="https://github.com/damachine/dmemcg-openrc/archive/refs/tags/v${PV}.tar.gz -> ${P}.tar.gz"
S="${WORKDIR}/${P}"

LICENSE="MIT"
SLOT="0"
KEYWORDS="amd64"

RDEPEND="sys-apps/openrc"

src_compile() {
	emake CC="$(tc-getCC)"
}

src_test() {
	emake check
}

src_install() {
	dosbin dmemcg-openrcd
	dobin dmem-run
	newinitd openrc/dmemcg-openrc dmemcg-openrc
	newconfd openrc/dmemcg-openrc.conf dmemcg-openrc
	dodoc README.md
	insinto "/usr/share/licenses/${PF}"
	doins LICENSE
}
