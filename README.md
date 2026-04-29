# STM32 Cooperative Scheduler v3.3

**Agendador cooperativo de tarefas para Cortex-M0+ com prioridade, grupos, deferred queue e sleep inteligente**

> Desenvolvido e validado em produção no firmware BWK17 (lavadoras Brastemp) sobre STM32C031K6T6 — 32 KB Flash / 6 KB RAM.

---

## Por que um scheduler cooperativo em vez de RTOS?

O STM32C031K6T6 é um microcontrolador Cortex-M0+ com **32 KB de Flash e 6 KB de RAM**. Um RTOS como FreeRTOS consome entre **3–5 KB de Flash** e exige **ao menos uma stack por tarefa** (~256–512 bytes cada), além de um TCB por tarefa (~40 bytes). Com 6 KB de RAM total e um firmware real com 8–10 "tarefas", o orçamento simplesmente não fecha.

| Recurso | FreeRTOS | Scheduler v3.3 |
|---------|----------|----------------|
| Flash (kernel) | ~3–5 KB | **~0 KB** (header-only) |
| RAM por tarefa | ~300–600 B (stack) | **~48 B** (struct `Tarefa`) |
| RAM total (8 tarefas) | ~2,5–5 KB | **~384 B** |
| Preempção | Sim | Não (cooperativo) |
| Determinismo temporal | Sim (prioridade real) | **Sim (por tick — dentro do ciclo)** |
| Heap | Necessário | **Zero** |
| Dependências externas | Kernel, port layer | **HAL_GetTick + 1 timer** |

### Modelo cooperativo: como funciona

Cada tarefa é uma **função C++ sem retorno** (`void (*)()`). O scheduler chama `executar()` no loop principal — cada tarefa roda até o final e cede o controle. Não há preempção. O desenvolvedor é responsável por manter as tarefas curtas.

```
loop principal:
  ┌─────────────────────────────────────────────────┐
  │  Agendador::instancia().executar()              │
  │                                                 │
  │  1. Drena fila deferred (callbacks de ISR)      │
  │  2. Coleta tarefas prontas no tick atual        │
  │  3. Ordena por prioridade (insertion sort)      │
  │  4. Executa em ordem de prioridade              │
  └─────────────────────────────────────────────────┘
  Agendador::instancia().dormirAteProximaTarefa()
    └─ WFI via TIM17 — CPU dorme até próxima tarefa
```

A combinação **cooperativo + WFI** entrega consumo de energia próximo a um RTOS tickless, sem nenhuma complexidade de kernel.

---

## Características Completas

### Núcleo

- **Template parametrizável**: `AgendadorT<N_TAREFAS, N_DEFERRED>` — tamanhos em compile-time, zero alocação dinâmica
- **Singleton Meyers**: `Agendador::instancia()` — zero indireção de ponteiro, inicialização ordenada
- **Header-only**: todo o scheduler em um único `.hpp`
- **AUTOSAR C++14**: sem `new`, sem `delete`, sem exceções, sem RTTI

### Escalonamento

- **Período configurável por tarefa** (1 ms a 4.294.967.295 ms)
- **Fase de início** (`faseMs`): offset inicial para distribuir tarefas no tempo e evitar colisões
- **Prioridade cooperativa** (0–255, 0 = maior urgência): resolve empates no mesmo tick via insertion sort estável
- **Recuperação de atraso** (`reagendarComRecuperacao`): catch-up limitado a `MAX_CATCHUP=16` ciclos — evita avalanche de execuções após bloqueio longo
- **Wrap-around seguro**: `tempoVencido()` usa aritmética com sinal em `uint32_t` — correto após ~49 dias de uptime

### Prioridade Cooperativa

```
Escala: 0 = maior urgência | 255 = menor urgência

   0 –  20  │ PRIORIDADE MÁXIMA  — segurança, watchdog
  21 –  99  │ PRIORIDADE ALTA    — comunicação, atuadores
 100 – 149  │ PRIORIDADE MÉDIA   — lógica de controle
 150 – 199  │ PRIORIDADE BAIXA   — display, visual
 200 – 255  │ PRIORIDADE MÍNIMA  — persistência, diagnóstico
```

> A prioridade resolve **empates no mesmo tick**. Não há preempção — a tarefa em execução roda até retornar.

### Grupos de Tarefas

Quatro grupos de suspensão mais `SEM_GRUPO`:

| Grupo | Valor | Comportamento |
|-------|-------|---------------|
| `SEGURANCA` | 0 | **Nunca suspenso** — protegido por hardware |
| `CONTROLE` | 1 | Suspensível |
| `DISPLAY` | 2 | Suspensível |
| `DIAGNOSTICO` | 3 | Suspensível |
| `SEM_GRUPO` | 0xFF | Sempre ativo |

```cpp
agendador.suspenderGrupo(GrupoTarefa::DISPLAY);   // suspende todo o grupo
agendador.retomarGrupo(GrupoTarefa::DISPLAY);      // retoma e reagenda automaticamente
```

### Fila Deferred (ISR → Task context)

Permite que ISRs (interrupções) enfileiem callbacks para execução no contexto do scheduler — seguro, sem race condition:

```cpp
// Dentro de uma ISR:
Agendador::instancia().despachar(minhaTarefaDeISR);

// O scheduler executa na próxima chamada a executar(),
// ANTES das tarefas periódicas do ciclo.
```

- Capacidade: `N_DEFERRED` callbacks (default: 4)
- Seção crítica com `__disable_irq()/__enable_irq()` — atômico
- Overflow contado em `contDeferredOverflow_` — diagnóstico sem crash
- Compactação automática após drenagem — slots reutilizados

### Sleep Inteligente via TIM17 + WFI

```cpp
// No loop principal:
while (true)
{
    agendador.executar();
    agendador.dormirAteProximaTarefa(); // CPU dorme até a próxima tarefa
}
```

O scheduler calcula o tempo até a próxima tarefa, arma o TIM17 como one-shot e executa `WFI`. A CPU acorda apenas quando há trabalho — sem polling ativo.

**Correção de race condition (FIX-1):** padrão canônico Cortex-M:
1. Arma TIM17
2. `__disable_irq()`
3. Verifica flag `g_schedulerSleepExpired`
4. Se 0: `__enable_irq()` + `__WFI()`
5. Se 1 (IRQ chegou entre armar e desabilitar): `__enable_irq()` sem dormir

Critério de sleep: `tempoMs >= SLEEP_MINIMO_MS` (2 ms). Abaixo disso, spin-wait — overhead de TIM17 não compensa.

### Watchdog por Tarefa

Cada tarefa marcada como `critica = true` possui um timeout individual de watchdog (`wdgTimeoutMs`). O scheduler verifica periodicamente via `alimentarWatchdog()`:

- Se todas as tarefas críticas executaram dentro do prazo: alimenta o IWDG hardware
- Se alguma falhou: dispara `callbackFalha` (uma única vez por evento) e **não alimenta o IWDG** — o hardware reseta o sistema

```cpp
// No loop principal:
agendador.alimentarWatchdog(); // chama IWDG::instancia().alimentar() internamente
```

### Limite de Execuções

Tarefas com vida finita:

```cpp
agendador.definirLimiteExecucoes(INDICE_TAREFA_NACK, 1U);
agendador.definirCallbackConcluido(INDICE_TAREFA_NACK, onNACKConcluido);
// A tarefa executa 1x, chama o callback e se desabilita automaticamente
```

### Diagnóstico em Tempo Real

Variáveis globais `volatile` para inspeção no debugger (Watch Window / Live Expressions):

| Variável | Tipo | Descrição |
|----------|------|-----------|
| `g_ultimaTarefaExecutada` | `uint8_t` | Índice da última tarefa executada — revela qual tarefa travou o sistema em caso de reset por IWDG |
| `g_ultimaExecMs` | `uint32_t` | Timestamp de início da última execução |
| `g_dbg_eficiencia` | `uint8_t` | % de ciclos em sleep (0–100) |
| `g_dbg_perdas` | `uint32_t` | Total de deadline misses acumuladas em todas as tarefas |

Estrutura `DiagnosticoTarefa` por índice:

```cpp
DiagnosticoTarefa d = agendador.obterDiagnostico(INDICE_TAREFA);
// d.jitterMaxMs    — pior jitter registrado (ms)
// d.totalExecucoes — total de execuções desde o reset
// d.perdas         — deadline misses desta tarefa
// d.wdgTimeoutMs   — timeout watchdog configurado
```

---

## Correções Aplicadas na v3.3

### BUG-A — Overflow em `obterEficiencia()`

**Problema:** `(contSleep_ * 100) / contLoop_` em `uint32_t` overflow quando `contSleep_` se aproxima de `UINT32_MAX` (~497 dias de operação contínua).

**Correção:** Cast para `uint64_t` apenas na operação crítica. Custo de emulação no M0+ aceitável — chamada esporádica.

```cpp
// v3.2 — overflow silencioso
uint8_t pct = (contSleep_ * 100U) / contLoop_;

// v3.3 — correto
uint64_t pct = (static_cast<uint64_t>(contSleep_) * 100ULL)
               / static_cast<uint64_t>(contLoop_);
```

### BUG-B — Timestamp errado em `reagendarComRecuperacao()`

**Problema:** `executar()` usava `agora` capturado na Fase 2 (antes de executar tarefas) para reagendar tarefas atrasadas. Tarefas anteriores no mesmo ciclo podiam demorar vários ms — o reagendamento ficava no passado, causando execuções imediatas em cascata.

**Correção:** `agoraPos = HAL_GetTick()` capturado **após** `t.funcao()`. `detectarAtraso()` continua usando `agora` da Fase 2 — correto conceitualmente (mede atraso no momento da coleta).

### BUG-C — Underflow em `idxDeferred_`

**Problema:** `idxDeferred_ -= nDeferred` sem verificação. ISRs podiam modificar `idxDeferred_` entre os dois `__disable_irq()`, causando underflow.

**Correção:** Guard seguro com clamp:

```cpp
const uint8_t atual = idxDeferred_;
const uint8_t sub   = (nDeferred <= atual) ? nDeferred : atual;
idxDeferred_ -= sub;
```

---

## Melhorias Aplicadas na v3.3

### IMP-H — Prioridade como parâmetro em `iniciar()`

**Problema (v3.2):** `adicionarTarefa()` chamava `iniciar()` e depois atribuía `prioridade` separadamente. Se `iniciar()` fosse chamado diretamente (sem `adicionarTarefa()`), a prioridade voltava ao padrão silenciosamente.

**Correção:** `iniciar()` recebe `prioridade` como parâmetro com default `PRIORIDADE_PADRAO`. `adicionarTarefa()` passa o valor diretamente — sem dupla atribuição.

### IMP-I — Redução de chamadas a `HAL_GetTick()`

`tsInicio` capturado antes de `t.funcao()` é reutilizado em `g_ultimaExecMs`. Reduz `HAL_GetTick()` de 4× para 3× por tarefa na Fase 4.

---

## Limitações Conhecidas

### LIM-1 — Leitura transitória de `obterEficiencia()` após wrap

`contLoop_` e `contSleep_` são `uint32_t`. Após ~497 dias de operação contínua (wrap), `obterEficiencia()` pode retornar um valor incorreto **apenas no ciclo imediatamente após o wrap**. Impacto: somente diagnóstico — zero efeito funcional. Correção não justificada para o ciclo de vida do produto.

---

## API de Referência

### Configuração de Tarefas

```cpp
// Adiciona ou reconfigura uma tarefa
void adicionarTarefa(
    uint8_t      indice,
    FuncaoTarefa funcao,
    uint32_t     periodoMs,
    uint32_t     faseMs     = 0U,
    bool         hab        = true,
    bool         isCritica  = false,
    GrupoTarefa  grupo      = GrupoTarefa::SEM_GRUPO,
    FuncaoFalha  onFalha    = nullptr,
    uint8_t      prioridade = SchedulerConfig::PRIORIDADE_PADRAO
);

// Ajusta período em runtime
void definirPeriodo(uint8_t indice, uint32_t periodoMs);

// Ajusta prioridade em runtime
void definirPrioridade(uint8_t indice, uint8_t prioridade);

// Habilita/desabilita tarefa (resets contadores ao habilitar)
void habilitarTarefa(uint8_t indice, bool hab);

// Força execução imediata na próxima chamada a executar()
void resetarTarefa(uint8_t indice);
```

### Watchdog por Tarefa

```cpp
// Define timeout individual (default: 2000 ms)
void definirWdgTimeout(uint8_t indice, uint32_t timeoutMs);

// Define timeout para todo um grupo
void definirWdgTimeoutGrupo(GrupoTarefa grupo, uint32_t timeoutMs);

// Desabilita watchdog por tarefa (modo debug — 60 s)
void desabilitarWdgPorTarefa();

// Alimenta IWDG se todas as tarefas críticas estão OK
// Retorna true se alimentou, false se alguma tarefa falhou
bool alimentarWatchdog();
```

### Grupos

```cpp
void suspenderGrupo(GrupoTarefa grupo); // SEGURANCA nunca é suspenso
void retomarGrupo(GrupoTarefa grupo);   // reagenda tarefas do grupo automaticamente
```

### Fila Deferred

```cpp
// Enfileira callback de ISR (thread-safe)
bool despachar(FuncaoTarefa f);

// Limpa toda a fila (use em reinicialização)
void reiniciarFila();

// Número de overflows desde o último reiniciarFila()
uint32_t obterContDeferredOverflow();
```

### Execução

```cpp
// Executa um ciclo completo (deferred + tarefas prontas)
ResultadoExecucao executar();

// Dorme até a próxima tarefa via TIM17 + WFI
// Retorna false se não há tarefas ou tempo < SLEEP_MINIMO_MS
bool dormirAteProximaTarefa();
```

### Diagnóstico

```cpp
// Retorna struct com todos os campos de diagnóstico de uma tarefa
DiagnosticoTarefa obterDiagnostico(uint8_t indice);

// Eficiência de sleep: % de ciclos em que dormiu (0–100)
uint8_t obterEficiencia();

// Deadline misses de uma tarefa específica
uint16_t obterPerdas(uint8_t indice);

// Soma de todas as perdas
uint32_t obterTotalPerdas();

// Pior jitter registrado (ms)
uint32_t obterJitterMax(uint8_t indice);

// Verifica se há tarefa pronta para executar agora
bool temTarefasPendentes();

// Tempo (ms) até a próxima tarefa
uint32_t proximoTempoExecucao();
```

---

## Uso Mínimo

### 1. Definir variáveis de diagnóstico (`main.cpp`)

```cpp
volatile uint8_t  g_ultimaTarefaExecutada = 0xFFU;
volatile uint32_t g_ultimaExecMs          = 0U;
volatile uint8_t  g_dbg_eficiencia        = 0U;
volatile uint32_t g_dbg_perdas            = 0U;
```

### 2. Registrar handler do TIM17 (`stm32c0xx_it.c`)

```c
volatile uint8_t g_schedulerSleepExpired = 0U;

void TIM17_IRQHandler(void)
{
    if (TIM17->SR & TIM_SR_UIF)
    {
        TIM17->SR &= ~TIM_SR_UIF;
        g_schedulerSleepExpired = 1U;
    }
}
```

### 3. Configurar e iniciar (`main.cpp`)

```cpp
#include "Scheduler_v3_3.hpp"

// Índices (enums recomendado)
constexpr uint8_t IDX_SENSOR   = 0U;
constexpr uint8_t IDX_CONTROLE = 1U;
constexpr uint8_t IDX_DISPLAY  = 2U;

int main()
{
    HAL_Init();
    SystemClock_Config();

    auto& sch = Agendador::instancia();

    sch.adicionarTarefa(IDX_SENSOR,   taskSensor,   500U,  0U, true, true,
                        GrupoTarefa::SEGURANCA,  onSensorFalha, 10U);

    sch.adicionarTarefa(IDX_CONTROLE, taskControle, 100U, 10U, true, false,
                        GrupoTarefa::CONTROLE,   nullptr,       50U);

    sch.adicionarTarefa(IDX_DISPLAY,  taskDisplay,  200U, 20U, true, false,
                        GrupoTarefa::DISPLAY,    nullptr,       150U);

    sch.definirWdgTimeout(IDX_SENSOR, 1500U); // tarefa crítica: timeout 1,5 s

    while (true)
    {
        sch.executar();
        sch.alimentarWatchdog();
        sch.dormirAteProximaTarefa();
    }
}
```

---

## Memória Utilizada

Com `AgendadorT<10, 4>` (default) sobre Cortex-M0+ com ARM Clang V6 `-Os`:

| Recurso | Tamanho |
|---------|---------|
| `struct Tarefa` | ~48 bytes |
| `AgendadorT<10, 4>` (RAM) | ~490 bytes |
| Código gerado (Flash) | ~1,2–1,8 KB |
| Stack em `executar()` | ~32 bytes (array local de índices) |
| Heap | **0 bytes** |

Para reduzir ao mínimo absoluto:

```cpp
using AgendadorMini = AgendadorT<4, 2>; // 4 tarefas, 2 deferred slots
// RAM: ~200 bytes
```

---

## Requisitos

| Componente | Versão mínima |
|------------|---------------|
| Compilador | ARM Clang V6 / GCC 7+ com C++14 |
| HAL | STM32C0xx HAL (fornece `HAL_GetTick()`) |
| Timer hardware | TIM17 disponível (usado para sleep) |
| IWDG | Opcional — necessário apenas se `alimentarWatchdog()` for usado |
| RAM | ~500 bytes para `AgendadorT<10,4>` |
| Flash | ~1,5 KB |

Portável para qualquer Cortex-M com `HAL_GetTick()` e um timer de 16 bits. Adaptações necessárias: substituir `TIM17` pelo timer disponível e ajustar `PSC_1KHZ` conforme o clock do APB.

---

## Histórico de Versões

| Versão | Principais mudanças |
|--------|---------------------|
| **v3.3** | BUG-A (overflow eficiência), BUG-B (timestamp reagendamento), BUG-C (underflow deferred); IMP-H (prioridade em `iniciar()`), IMP-I (HAL_GetTick 4×→3×) |
| **v3.2** | Prioridade cooperativa, insertion sort estável, `ordenarPorPrioridade()` |
| **v3.1** | BUG-2 (jitter só atraso de despacho), BUG-3 (catch-up limitado), BUG-4 (callback falha one-shot); `tempoVencido()` com aritmética signed |
| **v3.0** | Grupos de tarefas, suspensão/retomada, deferred queue, sleep TIM17+WFI |

---

## Licença

MIT — consulte o arquivo `LICENSE`.

---

*Desenvolvido para o firmware BWK17 (lavadoras Brastemp) pela equipe da Alpe Placas Eletrônicas / Metalúrgica Alado. Validado em produção sobre STM32C031K6T6.*
