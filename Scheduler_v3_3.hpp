


// ====================================================================
// ARQUIVO: Scheduler_v3.3.hpp (AUTOSAR C++14)
//
// Agendador cooperativo para STM32C031
// VERSAO 3.3 - Producao
//
// Correcoes aplicadas sobre v3.2:
//   [BUG-A] obterEficiencia(): overflow uint32_t corrigido com
//           uint64_t apenas na operacao critica. Chamada esporadica
//           — custo de emulacao no M0+ e aceitavel.
//
//   [BUG-B] executar() Fase 4: agora desatualizado em
//           reagendarComRecuperacao(). Corrigido com agoraPos =
//           HAL_GetTick() capturado APOS t.funcao(). detectarAtraso()
//           continua usando agora da Fase 2 — correto conceitualmente
//           (mede atraso em relacao ao tick de coleta, nao de execucao).
//
//   [BUG-C] executar() Fase 1: idxDeferred_ -= nDeferred sem
//           verificacao de underflow. Corrigido com guards seguro.
//
// Melhorias aplicadas sobre v3.2:
//   [IMP-H] iniciar(): prioridade como parametro com default
//           PRIORIDADE_PADRAO — elimina risco de reset silencioso.
//           adicionarTarefa() passa prioridade direto para iniciar().
//
//   [IMP-I] HAL_GetTick() reduzido de 4x para 3x por tarefa
//           na Fase 4: tsInicio reutilizado em g_ultimaExecMs.
//
// Todas as correcoes e melhorias de v3.1 e v3.2 mantidas.

// ************************ OBSERVACOES *******************************
// LIMITACOES CONHECIDAS:
//   [LIM-1] contLoop_ e contSleep_ sao uint32_t.
//           obterEficiencia() pode retornar leitura errada no ciclo
//           imediatamente apos wrap (~497 dias de operacao continua).
//           Impacto: somente diagnostico — nenhum efeito funcional.
//           Correcao nao justificada para o ciclo de vida do produto.


// ======================== PRIORIDADE DE TAREFAS ======================
// Scheduler_v3.3 com prioridade cooperativa
//
// Escala de prioridade:
//    0-9   = critica    (seguranca, watchdog)
//    10-49 = alta       (entrada do usuario, comunicacao)
//    50-99 = media      (logica de controle)
//    100-149 = baixa    (display, visual)
//    150-255 = minima   (persistencia, diagnostico)

// ================ SIMPLIFICANDO OS NIVEIS DE PRIORIDADE =============
// NIVEIS DE PRIORIDADE — Scheduler_v3.3
// Escala: 0 = maior urgencia | 255 = menor urgencia
//
//   0  -  20  | PRIORIDADE MAXIMA   — seguranca e controle critico
//  21  -  99  | PRIORIDADE ALTA     — comunicacao e atuadores
// 100  - 149  | PRIORIDADE MEDIA    — logica de controle
// 150  - 199  | PRIORIDADE BAIXA    — display e visual
// 200  - 255  | PRIORIDADE MINIMA   — persistencia e diagnostico
// ====================================================================

// LEMBRETE: prioridade resolve EMPATES no mesmo tick.
// Nao ha preempcao — tarefa em execucao roda ate retornar.
// ====================================================================

#ifndef SCHEDULER_V33_HPP
#define SCHEDULER_V33_HPP

#include "stm32c0xx.h"
#include "stm32c0xx_hal.h"
#include "WatchdogManager.h"
#include <cstdint>
#include <climits>

// ====================================================================
// LINKAGE EXTERNO
// Obrigatorio em stm32c0xx_it.c:
//
//   volatile uint8_t g_schedulerSleepExpired = 0U;
//
//   void TIM17_IRQHandler(void)
//   {
//       if (TIM17->SR & TIM_SR_UIF)
//       {
//           TIM17->SR &= ~TIM_SR_UIF;
//           g_schedulerSleepExpired = 1U;
//       }
//   }
// ====================================================================
extern "C" volatile uint8_t g_schedulerSleepExpired;

// ====================================================================
// VARIAVEIS DE DIAGNOSTICO
// Definir em main.cpp:
//
//   volatile uint8_t  g_ultimaTarefaExecutada = 0xFFU;
//   volatile uint32_t g_ultimaExecMs          = 0U;
//   volatile uint8_t  g_dbg_eficiencia        = 0U;
//   volatile uint32_t g_dbg_perdas            = 0U;
// ====================================================================
extern volatile uint8_t  g_ultimaTarefaExecutada;
extern volatile uint32_t g_ultimaExecMs;
extern volatile uint8_t  g_dbg_eficiencia;
extern volatile uint32_t g_dbg_perdas;

// ====================================================================
// NAMESPACE: SchedulerConfig
// ====================================================================
namespace SchedulerConfig
{
    constexpr std::uint8_t  MAX_TAREFAS_PADRAO    {10U};
    constexpr std::uint8_t  MAX_DEFERRED_PADRAO   {4U};
    constexpr std::uint8_t  MAX_CATCHUP           {16U};

    constexpr std::uint32_t PERIODO_INVALIDO      {0U};
    constexpr std::uint32_t PERIODO_MINIMO        {1U};

    constexpr std::uint16_t PERDAS_INICIAL        {0U};
    constexpr std::uint16_t PERDAS_MAXIMO         {0xFFFFU};

    constexpr std::uint32_t SLEEP_MINIMO_MS       {2U};

    constexpr std::uint32_t WDG_TIMEOUT_PADRAO_MS {2000U};
    constexpr std::uint32_t WDG_TIMEOUT_DEBUG_MS  {60000U};
    constexpr std::uint32_t LIMITE_INFINITO        {0U};

    constexpr std::uint8_t  INDICE_INVALIDO       {0xFFU};
    constexpr std::uint16_t PROGRAMA_INVALIDO     {0xFFFFU};

    constexpr std::uint8_t  PRIORIDADE_PADRAO     {128U};
    constexpr std::uint8_t  PRIORIDADE_MAXIMA     {0U};
    constexpr std::uint8_t  PRIORIDADE_MINIMA     {255U};

} // namespace SchedulerConfig

// ====================================================================
// NAMESPACE: SleepTimerTIM17
// ====================================================================
namespace SleepTimerTIM17
{
    constexpr std::uint32_t APB_CLOCK_HZ_ESPERADO {24000000U};
    constexpr std::uint32_t PSC_1KHZ              {23999U};
    constexpr std::uint32_t ARR_MAX               {65534U};
    constexpr std::uint32_t NVIC_PRIO             {3U};

    inline void armar(std::uint32_t ms) noexcept
    {
        g_schedulerSleepExpired = 0U;

        const std::uint32_t ms_clamp =
            (ms < 1U)      ? 1U     :
            (ms > ARR_MAX) ? ARR_MAX : ms;

        const std::uint16_t arr =
            static_cast<std::uint16_t>(ms_clamp - 1U);

        __HAL_RCC_TIM17_CLK_ENABLE();

        TIM17->CR1  = 0U;
        TIM17->PSC  = PSC_1KHZ;
        TIM17->ARR  = arr;
        TIM17->CNT  = 0U;
        TIM17->SR   = 0U;
        TIM17->DIER = TIM_DIER_UIE;
        TIM17->EGR  = TIM_EGR_UG;
        TIM17->SR   = 0U;

        NVIC_SetPriority(TIM17_IRQn, NVIC_PRIO);
        NVIC_ClearPendingIRQ(TIM17_IRQn);
        NVIC_EnableIRQ(TIM17_IRQn);

        TIM17->CR1 = TIM_CR1_OPM | TIM_CR1_CEN;
    }

    inline void parar() noexcept
    {
        NVIC_DisableIRQ(TIM17_IRQn);
        TIM17->CR1  = 0U;
        TIM17->DIER = 0U;
        TIM17->SR   = 0U;
    }

    // [FIX-1] Race condition corrigida — padrao canonico Cortex-M
    inline void dormirSeguro(std::uint32_t ms) noexcept
    {
        armar(ms);

        __disable_irq();

        if (g_schedulerSleepExpired == 0U)
        {
            __enable_irq();
            __WFI();
        }
        else
        {
            __enable_irq();
        }

        parar();
    }

} // namespace SleepTimerTIM17

// ====================================================================
// ENUM CLASS: GrupoTarefa
// ====================================================================
enum class GrupoTarefa : std::uint8_t
{
    SEGURANCA   = 0U,
    CONTROLE    = 1U,
    DISPLAY     = 2U,
    DIAGNOSTICO = 3U,
    SEM_GRUPO   = 0xFFU
};

constexpr std::uint8_t NUM_GRUPOS_SUSPENSAO {4U};

// ====================================================================
// tempoVencido(): wrap-around seguro para uint32_t
// ====================================================================
[[nodiscard]] static constexpr bool tempoVencido(
    std::uint32_t agora,
    std::uint32_t proximaExecucao) noexcept
{
    return static_cast<std::int32_t>(agora - proximaExecucao) >= 0;
}

// ====================================================================
// TIPOS DE FUNCAO
// ====================================================================
using FuncaoTarefa = void(*)();
using FuncaoFalha  = void(*)(std::uint8_t indiceTarefa);

// ====================================================================
// STRUCT: ResultadoExecucao
// ====================================================================
struct ResultadoExecucao
{
    std::uint8_t tarefasExecutadas    {0U};
    std::uint8_t deferredsProcessados {0U};
};

// ====================================================================
// STRUCT: DiagnosticoTarefa
// ====================================================================
struct DiagnosticoTarefa
{
    std::uint32_t periodoMs      {0U};
    std::uint32_t proximaMs      {0U};
    std::uint32_t ultimaExecMs   {0U};
    std::uint32_t totalExecucoes {0U};
    std::uint32_t jitterMaxMs    {0U};
    std::uint32_t wdgTimeoutMs   {0U};
    std::uint16_t perdas         {0U};
    GrupoTarefa   grupo          {GrupoTarefa::SEM_GRUPO};
    std::uint8_t  prioridade     {SchedulerConfig::PRIORIDADE_PADRAO};
    bool          habilitada     {false};
    bool          critica        {false};
    bool          valida         {false};
};

// ====================================================================
// STRUCT: InfoProximaTarefa
// ====================================================================
struct InfoProximaTarefa
{
    std::uint32_t tempoMs {UINT32_MAX};
    std::uint8_t  indice  {SchedulerConfig::INDICE_INVALIDO};
    bool          valida  {false};
};

// ====================================================================
// STRUCT: Tarefa
// [IMP-H] iniciar() recebe prioridade como parametro
// ====================================================================
struct Tarefa
{
    FuncaoTarefa           funcao                {nullptr};
    std::uint32_t          periodoMs             {SchedulerConfig::PERIODO_INVALIDO};
    std::uint32_t          proximaMs             {0U};
    bool                   habilitada            {false};
    volatile std::uint16_t perdas                {SchedulerConfig::PERDAS_INICIAL};
    bool                   critica               {false};
    std::uint32_t          ultimaExecMs          {0U};
    std::uint32_t          wdgTimeoutMs          {SchedulerConfig::WDG_TIMEOUT_PADRAO_MS};
    std::uint32_t          jitterMaxMs           {0U};
    std::uint32_t          totalExecucoes        {0U};
    GrupoTarefa            grupo                 {GrupoTarefa::SEM_GRUPO};
    FuncaoFalha            callbackFalha         {nullptr};
    bool                   callbackFalhaDisparado{false};
    std::uint32_t          limiteExecucoes       {SchedulerConfig::LIMITE_INFINITO};
    FuncaoFalha            callbackConcluido     {nullptr};
    std::uint8_t           prioridade            {SchedulerConfig::PRIORIDADE_PADRAO};

    // ------------------------------------------------------------------
    // [IMP-H] iniciar(): prioridade como parametro com default.
    // Elimina risco de reset silencioso ao chamar iniciar() diretamente.
    // adicionarTarefa() passa o valor recebido — sem dupla atribuicao.
    // ------------------------------------------------------------------
    void iniciar(FuncaoTarefa  f,
                 std::uint32_t periodo,
                 std::uint32_t faseMs      = 0U,
                 bool          hab         = true,
                 bool          isCritica   = false,
                 GrupoTarefa   grp         = GrupoTarefa::SEM_GRUPO,
                 std::uint8_t  prior       =
                     SchedulerConfig::PRIORIDADE_PADRAO) noexcept
    {
        funcao    = f;
        periodoMs = (periodo >= SchedulerConfig::PERIODO_MINIMO)
                        ? periodo
                        : SchedulerConfig::PERIODO_MINIMO;

        habilitada             = hab;
        proximaMs              = HAL_GetTick() + faseMs;
        perdas                 = SchedulerConfig::PERDAS_INICIAL;
        critica                = isCritica;
        ultimaExecMs           = HAL_GetTick();
        jitterMaxMs            = 0U;
        totalExecucoes         = 0U;
        grupo                  = grp;
        callbackFalha          = nullptr;
        callbackFalhaDisparado = false;
        limiteExecucoes        = SchedulerConfig::LIMITE_INFINITO;
        callbackConcluido      = nullptr;
        prioridade             = prior; // [IMP-H]
    }

    // ------------------------------------------------------------------
    void reagendar() noexcept
    {
        proximaMs += periodoMs;
    }

    // [BUG-3] Corrigido em v3.1 — mantido
    void reagendarComRecuperacao(std::uint32_t agora) noexcept
    {
        std::uint8_t iteracoes {0U};

        while (tempoVencido(agora, proximaMs)
               && iteracoes < SchedulerConfig::MAX_CATCHUP)
        {
            proximaMs += periodoMs;
            if (perdas < SchedulerConfig::PERDAS_MAXIMO) { ++perdas; }
            ++iteracoes;
        }

        if (tempoVencido(agora, proximaMs))
        {
            proximaMs = agora + periodoMs;
        }
    }

    // [BUG-2] Corrigido em v3.1 — mantido
    void registrarExecucao(std::uint32_t tsFim,
                           std::uint32_t planejado,
                           std::uint32_t tsInicio,
                           std::uint8_t  indice) noexcept
    {
        ultimaExecMs = tsFim;
        ++totalExecucoes;

        const std::uint32_t desvio =
            (tsInicio >= planejado) ? (tsInicio - planejado) : 0U;

        if (desvio > jitterMaxMs) { jitterMaxMs = desvio; }

        if (limiteExecucoes != SchedulerConfig::LIMITE_INFINITO)
        {
            if (totalExecucoes >= limiteExecucoes)
            {
                habilitada = false;
                if (callbackConcluido != nullptr)
                {
                    callbackConcluido(indice);
                    callbackConcluido = nullptr;
                }
            }
        }
    }

    [[nodiscard]] bool estaEmFalha(std::uint32_t agora) const noexcept
    {
        if (!critica || !habilitada) { return false; }
        return (agora - ultimaExecMs) > wdgTimeoutMs;
    }

    [[nodiscard]] bool estaConfigurada() const noexcept
    {
        return habilitada
            && (funcao    != nullptr)
            && (periodoMs != SchedulerConfig::PERIODO_INVALIDO);
    }

    [[nodiscard]] bool detectarAtraso(std::uint32_t agora) const noexcept
    {
        return static_cast<std::int32_t>(
            agora - (proximaMs + periodoMs)) >= 0;
    }

    void resetarCallbackFalha() noexcept
    {
        callbackFalhaDisparado = false;
    }

    void resetarPerdas() noexcept
    {
        perdas = SchedulerConfig::PERDAS_INICIAL;
    }

    ~Tarefa() = default;
};

// ====================================================================
// CLASSE: AgendadorT<N_TAREFAS, N_DEFERRED>
// ====================================================================
template<std::uint8_t N_TAREFAS  = SchedulerConfig::MAX_TAREFAS_PADRAO,
         std::uint8_t N_DEFERRED = SchedulerConfig::MAX_DEFERRED_PADRAO>
class AgendadorT
{
    static_assert(N_TAREFAS  >= 1U,  "N_TAREFAS deve ser >= 1");
    static_assert(N_TAREFAS  <= 32U, "N_TAREFAS deve ser <= 32");
    static_assert(N_DEFERRED >= 1U,  "N_DEFERRED deve ser >= 1");
    static_assert(N_DEFERRED <= 16U, "N_DEFERRED deve ser <= 16");

private:

    Tarefa        tarefas_[N_TAREFAS];
    bool          grupoSuspenso_[NUM_GRUPOS_SUSPENSAO] {false,false,false,false};
    FuncaoTarefa  filaDeferred_[N_DEFERRED]            {};
    std::uint8_t  idxDeferred_                         {0U};
    std::uint32_t contLoop_                            {0U};
    std::uint32_t contSleep_                           {0U};
    std::uint32_t contDeferredOverflow_                {0U};

    AgendadorT()  = default;
    ~AgendadorT() = default;
    AgendadorT(const AgendadorT&)            = delete;
    AgendadorT& operator=(const AgendadorT&) = delete;
    AgendadorT(AgendadorT&&)                 = delete;
    AgendadorT& operator=(AgendadorT&&)      = delete;

    // ----------------------------------------------------------------
    [[nodiscard]] static constexpr bool indiceValido(
        std::uint8_t indice) noexcept
    {
        return indice < N_TAREFAS;
    }

    // ----------------------------------------------------------------
    [[nodiscard]] bool grupoAtivo(const Tarefa& t) const noexcept
    {
        if (t.grupo == GrupoTarefa::SEGURANCA) { return true; }
        if (t.grupo == GrupoTarefa::SEM_GRUPO)  { return true; }

        const auto g = static_cast<std::uint8_t>(t.grupo);
        if (g >= NUM_GRUPOS_SUSPENSAO) { return true; }
        return !grupoSuspenso_[g];
    }

    // ----------------------------------------------------------------
    // ordenarPorPrioridade(): insertion sort estavel sobre indices.
    // O(n^2) com n <= 32 — adequado para embedded sem heap.
    // Estavel: mesma prioridade preserva ordem de indice original.
    // ----------------------------------------------------------------
    void ordenarPorPrioridade(std::uint8_t* indices,
                               std::uint8_t  n) const noexcept
    {
        for (std::uint8_t i {1U}; i < n; ++i)
        {
            const std::uint8_t chave      = indices[i];
            const std::uint8_t priorChave = tarefas_[chave].prioridade;
            std::int8_t        j          =
                static_cast<std::int8_t>(i) - 1;

            while (j >= 0 &&
                   tarefas_[indices[j]].prioridade > priorChave)
            {
                indices[j + 1U] = indices[static_cast<std::uint8_t>(j)];
                --j;
            }
            indices[static_cast<std::uint8_t>(j + 1)] = chave;
        }
    }

public:

    // ----------------------------------------------------------------
    [[nodiscard]] static AgendadorT& instancia() noexcept
    {
        static AgendadorT unicaInstancia;
        return unicaInstancia;
    }

    // ================================================================
    // CONFIGURACAO DE TAREFAS
    // ================================================================

    // ----------------------------------------------------------------
    // [IMP-H] adicionarTarefa(): passa prioridade para iniciar()
    // diretamente — sem dupla atribuicao, sem risco de reset silencioso.
    // ----------------------------------------------------------------
    void adicionarTarefa(
        std::uint8_t  indice,
        FuncaoTarefa  funcao,
        std::uint32_t periodoMs,
        std::uint32_t faseMs      = 0U,
        bool          hab         = true,
        bool          isCritica   = false,
        GrupoTarefa   grupo       = GrupoTarefa::SEM_GRUPO,
        FuncaoFalha   onFalha     = nullptr,
        std::uint8_t  prioridade  =
            SchedulerConfig::PRIORIDADE_PADRAO) noexcept
    {
        if (!indiceValido(indice)) { return; }

        // [IMP-H] prioridade vai direto para iniciar()
        tarefas_[indice].iniciar(funcao, periodoMs, faseMs,
                                 hab, isCritica, grupo,
                                 prioridade);

        tarefas_[indice].callbackFalha = onFalha;
    }

    // ----------------------------------------------------------------
    void definirPrioridade(std::uint8_t indice,
                            std::uint8_t prioridade) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].prioridade = prioridade;
        }
    }

    // ----------------------------------------------------------------
    [[nodiscard]] std::uint8_t obterPrioridade(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice))
        {
            return tarefas_[indice].prioridade;
        }
        return SchedulerConfig::PRIORIDADE_MINIMA;
    }

    // ----------------------------------------------------------------
    void definirCallbackFalha(std::uint8_t indice,
                               FuncaoFalha  onFalha) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].callbackFalha          = onFalha;
            tarefas_[indice].callbackFalhaDisparado = false;
        }
    }

    // ----------------------------------------------------------------
    void definirLimiteExecucoes(std::uint8_t  indice,
                                 std::uint32_t limite) noexcept
    {
        if (!indiceValido(indice)) { return; }
        tarefas_[indice].limiteExecucoes = limite;
        tarefas_[indice].totalExecucoes  = 0U;
    }

    // ----------------------------------------------------------------
    void definirCallbackConcluido(std::uint8_t indice,
                                   FuncaoFalha  onConcluido) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].callbackConcluido = onConcluido;
        }
    }

    // ----------------------------------------------------------------
    void definirPeriodo(std::uint8_t  indice,
                         std::uint32_t periodo) noexcept
    {
        if (!indiceValido(indice)) { return; }
        tarefas_[indice].periodoMs =
            (periodo >= SchedulerConfig::PERIODO_MINIMO)
                ? periodo
                : SchedulerConfig::PERIODO_MINIMO;
    }

    // ================================================================
    // FILA DEFERRED
    // ================================================================

    [[nodiscard]] bool despachar(FuncaoTarefa f) noexcept
    {
        if (f == nullptr) { return false; }

        __disable_irq();
        const bool fila_cheia = (idxDeferred_ >= N_DEFERRED);
        if (!fila_cheia)
        {
            filaDeferred_[idxDeferred_] = f;
            ++idxDeferred_;
        }
        else
        {
            ++contDeferredOverflow_;
        }
        __enable_irq();

        return !fila_cheia;
    }

    // ----------------------------------------------------------------
    void reiniciarFila() noexcept
    {
        __disable_irq();
        for (std::uint8_t i{0U}; i < N_DEFERRED; ++i)
        {
            filaDeferred_[i] = nullptr;
        }
        idxDeferred_          = 0U;
        contDeferredOverflow_ = 0U;
        __enable_irq();
    }

    // ================================================================
    // HABILITACAO E CONTROLE DE TAREFAS
    // ================================================================

    void habilitarTarefa(std::uint8_t indice, bool hab) noexcept
    {
        if (!indiceValido(indice)) { return; }

        tarefas_[indice].habilitada = hab;

        if (hab)
        {
            const std::uint32_t agora = HAL_GetTick();
            tarefas_[indice].proximaMs      =
                agora + tarefas_[indice].periodoMs;
            tarefas_[indice].ultimaExecMs   = agora;
            tarefas_[indice].totalExecucoes = 0U;
            tarefas_[indice].resetarPerdas();
            tarefas_[indice].resetarCallbackFalha();
        }
    }

    // ----------------------------------------------------------------
    void resetarTarefa(std::uint8_t indice) noexcept
    {
        if (!indiceValido(indice)) { return; }

        const std::uint32_t agora = HAL_GetTick();
        tarefas_[indice].proximaMs      = agora;
        tarefas_[indice].ultimaExecMs   = agora;
        tarefas_[indice].totalExecucoes = 0U;
        tarefas_[indice].resetarPerdas();
        tarefas_[indice].resetarCallbackFalha();
    }

    // ================================================================
    // CONTROLE DE GRUPOS
    // ================================================================

    void suspenderGrupo(GrupoTarefa grupo) noexcept
    {
        if (grupo == GrupoTarefa::SEGURANCA) { return; }
        if (grupo == GrupoTarefa::SEM_GRUPO)  { return; }

        const auto g = static_cast<std::uint8_t>(grupo);
        if (g < NUM_GRUPOS_SUSPENSAO)
        {
            grupoSuspenso_[g] = true;
        }
    }

    // ----------------------------------------------------------------
    void retomarGrupo(GrupoTarefa grupo) noexcept
    {
        const auto g = static_cast<std::uint8_t>(grupo);
        if (g >= NUM_GRUPOS_SUSPENSAO) { return; }

        grupoSuspenso_[g] = false;

        const std::uint32_t agora = HAL_GetTick();
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].grupo    == grupo
            &&  tarefas_[i].habilitada)
            {
                tarefas_[i].proximaMs    =
                    agora + tarefas_[i].periodoMs;
                tarefas_[i].ultimaExecMs = agora;
            }
        }
    }

    // ================================================================
    // WATCHDOG
    // ================================================================

    void definirWdgTimeout(std::uint8_t  indice,
                            std::uint32_t timeoutMs) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].wdgTimeoutMs = timeoutMs;
        }
    }

    // ----------------------------------------------------------------
    void definirWdgTimeoutGrupo(GrupoTarefa   grupo,
                                 std::uint32_t timeoutMs) noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].grupo == grupo)
            {
                tarefas_[i].wdgTimeoutMs = timeoutMs;
            }
        }
    }

    // ----------------------------------------------------------------
    void resetarTodasCriticas() noexcept
    {
        const std::uint32_t agora = HAL_GetTick();
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].critica)
            {
                tarefas_[i].ultimaExecMs           = agora;
                tarefas_[i].callbackFalhaDisparado = false;
            }
        }
    }

    // ----------------------------------------------------------------
    void desabilitarWdgPorTarefa() noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            tarefas_[i].wdgTimeoutMs =
                SchedulerConfig::WDG_TIMEOUT_DEBUG_MS;
        }
    }

    // ----------------------------------------------------------------
    [[nodiscard]] bool todasCriticasOk() const noexcept
    {
        const std::uint32_t agora = HAL_GetTick();
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].estaEmFalha(agora)) { return false; }
        }
        return true;
    }

    // ----------------------------------------------------------------
    // [BUG-4] Corrigido em v3.1 — mantido
    [[nodiscard]] bool alimentarWatchdog() noexcept
    {
        const std::uint32_t agora   = HAL_GetTick();
        bool                todasOk = true;

        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            if (tarefas_[i].estaEmFalha(agora))
            {
                if (tarefas_[i].callbackFalha          != nullptr
                &&  !tarefas_[i].callbackFalhaDisparado)
                {
                    tarefas_[i].callbackFalha(i);
                    tarefas_[i].callbackFalhaDisparado = true;
                }
                todasOk = false;
            }
        }

        if (todasOk)
        {
            IWDG_Singleton::instancia().alimentar();
            return true;
        }
        return false;
    }

    // ================================================================
    // NUCLEO DO AGENDADOR
    // ================================================================

    // ----------------------------------------------------------------
    [[nodiscard]] InfoProximaTarefa proximaExecucao() const noexcept
    {
        const std::uint32_t agora = HAL_GetTick();
        InfoProximaTarefa   info;

        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            const Tarefa& t = tarefas_[i];
            if (!t.estaConfigurada() || !grupoAtivo(t)) { continue; }

            if (tempoVencido(agora, t.proximaMs))
            {
                info.tempoMs = 0U;
                info.indice  = i;
                info.valida  = true;
                return info;
            }

            const std::uint32_t restante = t.proximaMs - agora;
            if (restante < info.tempoMs)
            {
                info.tempoMs = restante;
                info.indice  = i;
                info.valida  = true;
            }
        }
        return info;
    }

    // ----------------------------------------------------------------
    bool dormirAteProximaTarefa() noexcept
    {
        ++contLoop_;

        const InfoProximaTarefa info = proximaExecucao();

        if (!info.valida)                                      { return false; }
        if (info.tempoMs == 0U)                                { return false; }
        if (info.tempoMs < SchedulerConfig::SLEEP_MINIMO_MS)  { return false; }

        SleepTimerTIM17::dormirSeguro(info.tempoMs);

        ++contSleep_;
        return true;
    }

    // ================================================================
    // executar() — v3.3
    //
    // Fase 1: Drena fila deferred
    //   [BUG-1] Compactacao com clamp — sem out-of-bounds
    //   [BUG-C] idxDeferred_ com guard contra underflow
    //
    // Fase 2: Coleta indices de tarefas prontas no tick atual
    //   agora capturado uma vez — snapshot do ciclo
    //
    // Fase 3: Ordena por prioridade (insertion sort, estavel)
    //   Apenas se numProntas > 1 — sem trabalho desnecessario
    //
    // Fase 4: Executa na ordem de prioridade
    //   [BUG-B] agoraPos = HAL_GetTick() apos t.funcao()
    //           usado em reagendarComRecuperacao() — timestamp correto
    //   [IMP-I] tsInicio reutilizado em g_ultimaExecMs
    //           HAL_GetTick() reduzido de 4x para 3x por tarefa
    // ================================================================
    ResultadoExecucao executar() noexcept
    {
        ResultadoExecucao resultado;

        // ============================================================
        // Fase 1 — Deferred calls
        // ============================================================

        // Snapshot atomico do indice atual
        __disable_irq();
        const std::uint8_t nDeferred = idxDeferred_;
        __enable_irq();

        // Drena os N callbacks coletados no snapshot
        for (std::uint8_t i{0U}; i < nDeferred; ++i)
        {
            if (filaDeferred_[i] != nullptr)
            {
                filaDeferred_[i]();
                filaDeferred_[i] = nullptr;
                ++resultado.deferredsProcessados;
            }
        }

        // [BUG-C] + [BUG-1] Compacta fila com guards de underflow e clamp
        __disable_irq();
        {
            // [BUG-C] Guard contra underflow:
            // idxDeferred_ pode ter sido modificado por ISR entre os
            // dois __disable_irq(). Nunca subtrair mais do que o atual.
            const std::uint8_t atual = idxDeferred_;
            const std::uint8_t sub   = (nDeferred <= atual)
                                       ? nDeferred
                                       : atual;
            idxDeferred_ -= sub;

            if (idxDeferred_ > 0U)
            {
                // [BUG-1] Clamp: itens novos adicionados por ISR durante
                // a drenagem. Garante que o indice de origem nDeferred+i
                // nao ultrapasse N_DEFERRED.
                const std::uint8_t espaco =
                    (nDeferred < N_DEFERRED)
                        ? static_cast<std::uint8_t>(N_DEFERRED - nDeferred)
                        : 0U;

                const std::uint8_t novos =
                    (idxDeferred_ <= espaco)
                        ? idxDeferred_
                        : espaco; // clamp

                for (std::uint8_t i{0U}; i < novos; ++i)
                {
                    filaDeferred_[i]             = filaDeferred_[nDeferred + i];
                    filaDeferred_[nDeferred + i] = nullptr;
                }

                idxDeferred_ = novos; // contagem real apos compactacao
            }
        }
        __enable_irq();

        // ============================================================
        // Fase 2 — Coleta tarefas prontas (snapshot do tick)
        // ============================================================

        // agora e o timestamp canonico do ciclo.
        // detectarAtraso() usa este valor — mede atraso em relacao
        // ao momento de coleta, nao de execucao. Intencional.
        const std::uint32_t agora {HAL_GetTick()};

        // [RSC-1] Array local em stack — N_TAREFAS <= 32, max 32 bytes
        std::uint8_t prontas[N_TAREFAS];
        std::uint8_t numProntas {0U};

        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            const Tarefa& t = tarefas_[i];
            if (!t.estaConfigurada() || !grupoAtivo(t)) { continue; }

            if (tempoVencido(agora, t.proximaMs))
            {
                prontas[numProntas] = i;
                ++numProntas;
            }
        }

        // ============================================================
        // Fase 3 — Ordena por prioridade
        // ============================================================

        // Insertion sort estavel — skip se <= 1 tarefa pronta
        if (numProntas > 1U)
        {
            ordenarPorPrioridade(prontas, numProntas);
        }

        // ============================================================
        // Fase 4 — Executa na ordem de prioridade
        // ============================================================

        for (std::uint8_t k{0U}; k < numProntas; ++k)
        {
            const std::uint8_t i = prontas[k];
            Tarefa&             t = tarefas_[i];

            const std::uint32_t planejado = t.proximaMs;

            // detectarAtraso com agora da Fase 2 — correto:
            // mede se a tarefa ja perdeu um periodo completo no
            // momento em que foi coletada, independente de quando
            // sera executada dentro do ciclo.
            const bool atrasouMuito = t.detectarAtraso(agora);

            if (t.funcao != nullptr)
            {
                // [IMP-I] tsInicio capturado uma vez — reutilizado
                // em g_ultimaExecMs. Reduz HAL_GetTick() de 4x para 3x
                // por tarefa na Fase 4.
                const std::uint32_t tsInicio = HAL_GetTick();

                // Grava diagnostico ANTES de executar —
                // se IWDG resetar durante t.funcao(), Watch Window
                // mostra exatamente qual tarefa travou o sistema.
                g_ultimaTarefaExecutada = i;
                g_ultimaExecMs          = tsInicio; // [IMP-I]

                t.funcao(); // <<< executa a tarefa do usuario

                // tsFim capturado APOS execucao — para jitter e ultimaExecMs
                const std::uint32_t tsFim = HAL_GetTick();

                // [BUG-2] tsInicio passado separado de tsFim:
                // jitter = tsInicio - planejado (so atraso de despacho)
                // nao inclui duracao de execucao da tarefa
                t.registrarExecucao(tsFim, planejado, tsInicio, i);

                ++resultado.tarefasExecutadas;
            }

            if (t.habilitada) // pode ter sido desabilitada por limiteExecucoes
            {
                if (atrasouMuito)
                {
                    // [BUG-B] agoraPos = timestamp APOS execucao da tarefa.
                    // Tarefas anteriores do ciclo podem ter demorado varios ms.
                    // Usar agora da Fase 2 causaria reagendamento no passado.
                    const std::uint32_t agoraPos = HAL_GetTick();
                    t.reagendarComRecuperacao(agoraPos);
                }
                else
                {
                    // reagendar() apenas soma periodoMs — nao precisa de agora
                    t.reagendar();
                }
            }
        }

        // Atualiza diagnostico global — [IMP-F] herdado de v3.1
        g_dbg_eficiencia = obterEficiencia();
        g_dbg_perdas     = obterTotalPerdas();

        return resultado;
    }

    // ================================================================
    // CONSULTA E ESTATISTICAS
    // ================================================================

    // ----------------------------------------------------------------
    // [BUG-A] obterEficiencia(): corrigido com uint64_t.
    // Eliminado overflow silencioso de uint32_t em sistemas 24/7.
    // uint64_t usado apenas nesta operacao — emulado no M0+ mas
    // chamada e esporadica (a cada ciclo do loop principal, ~100ms).
    // ----------------------------------------------------------------
    [[nodiscard]] std::uint8_t obterEficiencia() const noexcept
    {
        if (contLoop_ == 0U) { return 0U; }

        // [BUG-A] Cast para uint64_t antes da multiplicacao —
        // elimina overflow quando contSleep_ se aproxima de UINT32_MAX
        const std::uint64_t pct =
            (static_cast<std::uint64_t>(contSleep_) * 100ULL)
            / static_cast<std::uint64_t>(contLoop_);

        // Saturacao em 100 — defensivo contra imprecisao
        return static_cast<std::uint8_t>(
            (pct > 100ULL) ? 100U : static_cast<std::uint8_t>(pct));
    }

    // ----------------------------------------------------------------
    void resetarEstatisticas() noexcept
    {
        contLoop_  = 0U;
        contSleep_ = 0U;
        resetarTodasPerdas();
    }

    // ----------------------------------------------------------------
    [[nodiscard]] DiagnosticoTarefa obterDiagnostico(
        std::uint8_t indice) const noexcept
    {
        if (!indiceValido(indice))
        {
            return DiagnosticoTarefa{};
        }

        const Tarefa& t = tarefas_[indice];
        DiagnosticoTarefa d;
        d.periodoMs      = t.periodoMs;
        d.proximaMs      = t.proximaMs;
        d.ultimaExecMs   = t.ultimaExecMs;
        d.totalExecucoes = t.totalExecucoes;
        d.jitterMaxMs    = t.jitterMaxMs;
        d.wdgTimeoutMs   = t.wdgTimeoutMs;
        d.perdas         = t.perdas;
        d.grupo          = t.grupo;
        d.prioridade     = t.prioridade;
        d.habilitada     = t.habilitada;
        d.critica        = t.critica;
        d.valida         = true;
        return d;
    }

    // ----------------------------------------------------------------
    [[nodiscard]] const Tarefa* obterTarefa(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice)) { return &tarefas_[indice]; }
        return nullptr;
    }

    [[nodiscard]] bool estaHabilitada(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice))
        {
            return tarefas_[indice].habilitada;
        }
        return false;
    }

    [[nodiscard]] std::uint16_t obterPerdas(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice)) { return tarefas_[indice].perdas; }
        return SchedulerConfig::PERDAS_INICIAL;
    }

    [[nodiscard]] std::uint32_t obterTotalPerdas() const noexcept
    {
        std::uint32_t total {0U};
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            total += tarefas_[i].perdas;
        }
        return total;
    }

    [[nodiscard]] std::uint32_t obterJitterMax(
        std::uint8_t indice) const noexcept
    {
        if (indiceValido(indice))
        {
            return tarefas_[indice].jitterMaxMs;
        }
        return 0U;
    }

    [[nodiscard]] std::uint32_t obterContDeferredOverflow() const noexcept
    {
        return contDeferredOverflow_;
    }

    [[nodiscard]] bool temTarefasPendentes() const noexcept
    {
        return proximaExecucao().tempoMs == 0U;
    }

    [[nodiscard]] std::uint32_t proximoTempoExecucao() const noexcept
    {
        return proximaExecucao().tempoMs;
    }

    void resetarPerdas(std::uint8_t indice) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].resetarPerdas();
        }
    }

    void resetarTodasPerdas() noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            tarefas_[i].resetarPerdas();
        }
    }

    void resetarCallbackFalha(std::uint8_t indice) noexcept
    {
        if (indiceValido(indice))
        {
            tarefas_[indice].resetarCallbackFalha();
        }
    }

    void resetarTodosCallbacksFalha() noexcept
    {
        for (std::uint8_t i{0U}; i < N_TAREFAS; ++i)
        {
            tarefas_[i].resetarCallbackFalha();
        }
    }

}; // class AgendadorT

// ====================================================================
// ALIAS: Agendador
// ====================================================================
using Agendador = AgendadorT<SchedulerConfig::MAX_TAREFAS_PADRAO,
                              SchedulerConfig::MAX_DEFERRED_PADRAO>;

#endif // SCHEDULER_V33_HPP




















