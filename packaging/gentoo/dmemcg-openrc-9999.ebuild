EAPI=8

EGIT_REPO_URI="https://github.com/damachine/dmemcg-openrc.git"

inherit git-r3 toolchain-funcs

DESCRIPTION="VRAM protection broker for OpenRC using DMEM low"
HOMEPAGE="https://github.com/damachine/dmemcg-openrc"

LICENSE="MIT"
SLOT="0"
KEYWORDS=""

RDEPEND="
	acct-group/video
	sys-apps/openrc
"

src_compile() {
	emake CC="$(tc-getCC)"
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
