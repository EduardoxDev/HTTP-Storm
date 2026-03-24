FROM ubuntu:22.04

# Instalar dependências
RUN apt-get update && apt-get install -y \
    cmake \
    build-essential \
    libssl-dev \
    zlib1g-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Criar diretório de trabalho
WORKDIR /app

# Copiar arquivos do projeto
COPY . .

# Criar diretório de build e compilar
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . -j$(nproc)

# Expor porta (ajuste se necessário)
EXPOSE 8080

# Comando para executar
CMD ["./build/http_server"]