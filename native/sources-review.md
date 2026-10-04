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
