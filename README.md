# PS4 Homebrew Store

Loja web estática para organizar downloads de **homebrew e arquivos autorizados**.

## Recursos
- Catálogo pesquisável
- Filtro PKG / ELF / BIN
- Download direto por URL
- Adição local de URL pelo navegador
- Catálogo público em `apps.json`
- HTML/CSS/JS puro

## Adicionar um item para todos
Edite `apps.json` e adicione um objeto com `name`, `type`, `description` e `url`.

O botão **Adicionar URL** salva somente no `localStorage` daquele navegador. Uma página estática não consegue enviar um PKG da internet para este repositório sem um backend ou autenticação.

## Uso responsável
Este projeto não inclui jogos comerciais, conteúdo pirateado, exploits ou ferramentas para contornar proteções do console. Use somente arquivos que você tenha direito de usar e distribuir.

## Publicar
Ative GitHub Pages nas configurações do repositório usando a branch `main` e a pasta raiz.
