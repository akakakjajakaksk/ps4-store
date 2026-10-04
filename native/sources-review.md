# Fontes de PKG para Peppy Store

Conferência de 2026-10-04. A loja instala pacotes base completos de PS4;
listas de jogos e páginas de hospedagem exigem conferência do arquivo final.
Esta revisão consulta índices públicos e exemplos de download. Ela não
verifica todos os arquivos de cada site.

| Fonte indicada | Evidência disponível | Resultado nesta integração |
| --- | --- | --- |
| [Pippo .exFAT](https://pippo26442999.github.io/.exFAT/) | 777 registros no JSON, todos com identificadores PPSA de PS5. | Nenhuma entrada para o catálogo PS4. |
| [DLPS](https://dlpsgame.com/category/ps4/) | Categoria com 20 posts por página e 324 páginas. The Rumble Fish 2 aponta para intermediários; `/archives/47074` retornou HTTP 403. | Nenhum binário verificável no exemplo consultado. |
| [PFS Library](https://pfs-library.vercel.app/) | A página indica mudança para `pfs-library.xetdy-am.workers.dev`, que retornou HTTP 404. | Catálogo indisponível no endereço indicado. |
| [SuperPSX](https://www.superpsx.com/) | O exemplo TABS anuncia base e update separados. Viking exige Turnstile; Mocha fornece metadados sem link binário no HTML consultado. | Essas páginas precisam de um resolvedor próprio; não entram como URLs de PKG. |
| [MTPS4](https://mtps4store380.blogspot.com/?m=1) | JSON público com 2.171 entradas e 3.434 links. Há 18 URLs com nome `.pkg`: 14 páginas MediaFire, três links Akirabox assinados e um FileKeeper. A página Akirabox consultada retornou HTTP 403. | Oito pacotes base integrados via MediaFire: Agony e sete conversões de PS2 para PS4. |

O feed Blogger do MTPS4 contém uma postagem de imagens; o catálogo efetivo
é um JSON carregado pela página. A conferência usa a versão
[`cb3244e`](https://github.com/Mtgames38oficial/jogosps4pkg/blob/cb3244edfa2825627320efff0db5701f7493a163/jogosps4pkg.json).
Nomes com `.pkg` na URL não comprovam que a resposta seja um pacote, e
tamanhos arredondados da lista não substituem o tamanho real do servidor.

## Arquivos conferidos

Foram lidos apenas os primeiros 1.080 bytes de cada arquivo, com pedidos
HTTP Range. Nenhum jogo completo foi baixado durante essa revisão.

| Arquivo | Content ID | Tamanho real | Cabeçalho | Decisão |
| --- | --- | --- | --- | --- |
| Agony | `UP2047-CUSA10216_00-AGONY666AMERICAS` | 10.518.134.784 bytes | Magic `7f434e54`, tipo `0x1A`, flags `0x0A000000`. | Pacote base do catálogo. |
| Yet Another Zombie Defense HD | `UP2387-CUSA18354_00-YAZDHD0000000000` | 83.492.864 bytes | Tipo `0x1A`, flags `0x62300000` com bits de patch. | Recusado como pacote base, mesmo estando listado como “Game”. |

Nos dois exemplos, o tamanho de pacote em `0x430` coincide com o total de
`Content-Range`. Os campos são documentados no
[`PkgReader`](https://github.com/maxton/LibOrbisPkg/blob/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/PKG/PkgReader.cs)
e os tipos/flags em
[`Enums`](https://github.com/maxton/LibOrbisPkg/blob/643477263b2644e0803e0f58b8726ea4e3f3b7d4/LibOrbisPkg/PKG/Enums.cs).
O tipo `0x1A` sozinho também pode identificar um patch ou remaster.

Os outros sete pacotes conferidos têm magic `7f434e54`, tipo `0x1A`, flags
`0x0A000000` e tamanho declarado igual ao total de HTTP Range. Seus
identificadores SLES/SLUS pertencem às conversões de PS2 para PS4; não são
PKGs nativos de jogos PS4. O cabeçalho e a procedência foram revisados juntos.

| Conversão | Title ID do cabeçalho | Tamanho real em bytes |
| --- | --- | --- |
| Futurama | SLES51507 | 5.250.744.320 |
| Need for Speed: Most Wanted | SLUS21351 | 3.742.892.032 |
| Need for Speed: Carbon | SLUS21494 | 3.729.260.544 |
| Pica-Pau | SLES50612 | 443.547.648 |
| Black | SLUS21376 | 1.359.675.392 |
| Area 51 | SLES52570 | 3.543.728.128 |
| Armored Core 2: Another Age | SLUS20249 | 3.815.243.776 |

Pica-Pau aparece com SLES50613 na lista original, mas o Content ID conferido
tem Title ID SLES50612. A entrada usa o identificador do arquivo recebido.

O catálogo guarda a página estável do MediaFire. A transferência consulta
essa página novamente, lê seu botão de download e usa o endereço HTTPS do
CDN naquele momento. A página nunca é gravada como PKG. Sites com captcha,
arquivos compactados, partes e links expirados precisam de tratamento
específico antes de disponibilizar a instalação.

A conferência do cabeçalho não valida a assinatura ou todas as estruturas
internas, e não comprova instalação no firmware 13.52. A transferência
completa continua verificando tamanho, magic e SHA-256 quando disponível.

## Fontes adicionais da Build 102

| Fonte indicada | Resultado da conferência de 2026-10-04 |
| --- | --- |
| [PKG-Zone](https://pkg-zone.com/) | Página e catálogo `api.pkg-zone.com/store.db` retornaram HTTP 500, inclusive com o User-Agent documentado da HB-Store. A HB-Store já está no catálogo por release oficial no GitHub. Itemzflow e PS4-Xplorer não foram adicionados: os espelhos consultados retornaram HTTP 403. |
| [Brewology](https://brewology.com/) | A página consultada prioriza homebrews de outras plataformas. IRISMAN, webMAN MOD e CFW/HFW de PS3 não são pacotes instaláveis neste aplicativo PS4. |
| [PSX-Place](https://www.psx-place.com/resources/categories/ps4-homebrew.42/) | O recurso [ioQuake3 PS4](https://www.psx-place.com/resources/ioquake3-ps4.1717/) aponta para o autor Mayo1970. Foram incluídas cinco builds da release 1.8, com dados de jogo externos explicitados. |
| [FPKGi](https://github.com/ItsJokerZz/FPKGi/releases/tag/v1.10.0) | Aplicativo PS4/PS5 do autor, release fixa v1.10.0, incluído por pacote PS4 conferido. Esta entrada instala o cliente; suas listas de jogos não foram importadas. |
| [GameBaTo](https://gamebatoapp.ir/home/en/) | Cliente PS4 incluído a partir do link público `/home/app.pkg`. O site anuncia firmware 5.05 a 12.0, sem comprovar 13.52. A página não publica versão/checksum: o catálogo mostra “site sem versão” e fixa o SHA256 calculado nesta revisão. |
| PKGi / NoPayStation | As opções indicadas para PS3, PS Vita e PSP ficam fora do catálogo PS4. A Peppy não integra downloads de servidores da PlayStation. |
| [Vimm's Lair](https://vimm.net/vault/PS4) | O endereço PS4 consultado retornou HTTP 404. Discos, ISOs e pastas de PS3 não são PKGs base instaláveis de PS4. |
| [Internet Archive](https://archive.org/details/ps4-fpkg-collection-english-h) | Incluídos três arquivos da coleção H após conferir metadados e cabeçalhos. A coleção declara releases modificadas, com atualizações mescladas e conteúdo removido; também avisa possíveis limitações no PS4 Pro. Esses avisos aparecem nas entradas. |
| [Romsfun](https://romsfun.com/download/sonic-mania-40290) | Sonic Mania leva a uma página 1fichier com link temporário. Nenhum binário direto foi conferido; não foi incluído como PKG instalável. |
| [Romspure](https://romspure.cc/roms/sony-playstation-4/) | O exemplo Puyo Puyo Tetris anuncia PKG, mas a página consultada não fornece um arquivo direto verificável. |

As quatro novas releases oficiais iniciais são FPKGi, PS4 Cheats Manager,
NP2kai PS4 e mGBA PS4. Os arquivos vieram dos repositórios dos próprios
autores, com tag fixa; Cheats Manager não publica SHA256, enquanto as outras
três releases publicam. NP2kai e mGBA precisam de dados externos descritos
nas respectivas entradas. Firmware 13.52 permanece sem teste no console.

| Arquivo adicionado | Title ID conferido | Tamanho em bytes |
| --- | --- | --- |
| FPKGi v1.10.0 | PKGI13337 | 85.458.944 |
| PS4 Cheats Manager v1.2.2 | CHTM00777 | 19.202.048 |
| NP2kai v1.0 | BREW00984 | 9.109.504 |
| mGBA ps4-v0.1.0 | MGBA00001 | 6.619.136 |
| ioQuake3: Quake III Arena 1.8 | QUAK03000 | 13.762.560 |
| ioQuake3: Team Arena 1.8 | QUAK03001 | 13.762.560 |
| ioQuake3: Open Arena 1.8 | QUAK03002 | 13.762.560 |
| ioQuake3: Classic 1.8 | QUAK03003 | 13.107.200 |
| ioQuake3: Elite Force 1.8 | QUAK03004 | 13.762.560 |
| GameBaTo, arquivo do site em 2026-10-04 | GBTX00001 | 23.068.672 |
| Hotline Miami, versão modificada 1.01 | CUSA00486 | 161.939.456 |
| Hotline Miami 2, versão modificada 1.01 | CUSA00368 | 391.512.064 |
| Hollow Knight, versão modificada 1.02 | CUSA13285 | 1.237.516.288 |

Todos esses cabeçalhos têm magic `7f434e54`, tipo `0x1A`, flags
`0x0A000000` e tamanho declarado igual ao total de HTTP Range. A fonte e
o formato PS4 também foram conferidos; o instalador não aceita atualizações
ou DLCs como pacotes base.

As cinco builds de ioQuake3 não incluem os arquivos `.pk3` dos jogos. O
autor documenta os diretórios em
[INSTALLATION.md da tag 1.8](https://github.com/Mayo1970/ioQuake3-PS4/blob/1.8/INSTALLATION.md).
Open Arena usa dados gratuitos, também fornecidos à parte. As outras builds
precisam dos dados correspondentes; instalar o motor sozinho não basta.

Somente o cliente GameBaTo foi baixado integralmente para calcular SHA256:
`529a33a55722c2488c9b190da6eb6132997903c3253091e576538785d3c937bb`.
Nenhum jogo completo foi baixado nesta revisão. Como `app.pkg` é mutável,
uma mudança no arquivo do servidor exige atualizar a evidência e o hash.

Internet Archive mantém URLs estáveis da coleção. Os destinos observados
foram `ia800705.us.archive.org`, `dn721707.ca.archive.org` e
`dn760105.eu.archive.org`; somente esses hosts e `archive.org` são aceitos,
com rota limitada à coleção revisada e um único basename `.pkg`.
Um novo CDN exige nova revisão. GameBaTo aceita somente o endereço exato do
cliente. Cada provedor permanece isolado nos redirecionamentos HTTPS.

O catálogo final contém 36 entradas: 24 releases oficiais, oito pacotes
MediaFire, três pacotes Internet Archive e um cliente GameBaTo do site.
Não é uma importação completa dos sites. Nenhum PKG enviado pelo usuário foi
incluído nesta revisão, pois os anexos recebidos até agora são fotografias.
