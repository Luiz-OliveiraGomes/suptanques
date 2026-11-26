#include <iostream>     /* cerr */
#include <algorithm>
#include <cstdint>      /* uint16_t */
#include "supservidor.h"

using namespace std;

/* ========================================
   CLASSE SUPSERVIDOR
   ======================================== */

/// Construtor
SupServidor::SupServidor()
  : Tanks()
  , server_on(false)
  , LU()
  , thr_server()
  , sock_server()
{
  // Inicializa a biblioteca de sockets
  mysocket_status status = mysocket::init();
  if (status != mysocket_status::SOCK_OK)
  {
    cerr <<  "Biblioteca mysocket nao pode ser inicializada";
    exit(-1);
  }
}

/// Destrutor
SupServidor::~SupServidor()
{
  // Deve parar a thread do servidor
  server_on = false;

  // Fecha todos os sockets dos clientes
  for (auto& U : LU) U.close();
  // Fecha o socket de conexoes
  sock_server.close();

  // Espera o fim da thread do servidor
  if (thr_server.joinable()) thr_server.join();

  // Encerra a biblioteca de sockets
  mysocket::end();
}

/// Liga o servidor
bool SupServidor::setServerOn()
{
  // Se jah estah ligado, nao faz nada
  if (server_on) return true;

  // Liga os tanques
  setTanksOn();

  // Indica que o servidor estah ligado a partir de agora
  server_on = true;

  try
  {
    // Coloca o socket de conexoes em escuta
    mysocket_status status = sock_server.listen(SUP_PORT);
    // Em caso de erro, gera excecao
    if (status != mysocket_status::SOCK_OK) throw 1;

    // Lanca a thread do servidor que comunica com os clientes
    try
    {
      thr_server = std::thread([this](){this->thr_server_main();});
    }
    catch(...)
    {
      throw 2;
    }
    // Em caso de erro, gera excecao
    if (!thr_server.joinable()) throw 2;
  }
  catch(int i)
  {
    cerr << "Erro " << i << " ao iniciar o servidor\n";

    // Deve parar a thread do servidor
    server_on = false;

    // Fecha o socket do servidor
    sock_server.close();
    setTanksOff();

    return false;
  }

  // Tudo OK
  return true;
}

/// Desliga o servidor
void SupServidor::setServerOff()
{
  // Se jah estah desligado, nao faz nada
  if (!server_on) return;

  // Deve parar a thread do servidor
  server_on = false;

  // Fecha todos os sockets dos clientes
  for (auto& U : LU) U.close();
  // Fecha o socket de conexoes
  sock_server.close();

  // Espera pelo fim da thread do servidor
  if (thr_server.joinable()) thr_server.join();
  // Faz o identificador da thread apontar para thread vazia
  thr_server = std::thread();

  // Desliga os tanques
  setTanksOff();
}

/// Leitura do estado dos tanques
void SupServidor::readStateFromSensors(SupState& S) const
{
  // Estados das valvulas: OPEN, CLOSED
  S.V1 = v1isOpen();
  S.V2 = v2isOpen();
  // Niveis dos tanques: 0 a 65535
  S.H1 = hTank1();
  S.H2 = hTank2();
  // Entrada da bomba: 0 a 65535
  S.PumpInput = pumpInput();
  // Vazao da bomba: 0 a 65535
  S.PumpFlow = pumpFlow();
  // Estah transbordando (true) ou nao (false)
  S.ovfl = isOverflowing();
}

/// Leitura e impressao em console do estado da planta
void SupServidor::readPrintState() const
{
  if (tanksOn())
  {
    SupState S;
    readStateFromSensors(S);
    S.print();
  }
  else
  {
    cout << "Tanques estao desligados!\n";
  }
}

/// Impressao em console dos usuarios do servidor
void SupServidor::printUsers() const
{
  for (const auto& U : LU)
  {
    cout << U.login << '\t'
         << "Admin=" << (U.isAdmin ? "SIM" : "NAO") << '\t'
         << "Conect=" << (U.isConnected() ? "SIM" : "NAO") << '\n';
  }
}

/// Adicionar um novo usuario
bool SupServidor::addUser(const string& Login, const string& Senha,
                             bool Admin)
{
  // Nao pode ser chamada com servidor em funcionamento
  if (serverOn())
  {
    cerr << "Nao pode adicionar usuarios com servidor em funcionamento!\n";
    return false;
  }

  // Testa os dados do novo usuario
  if (Login.size()<6 || Login.size()>12) return false;
  if (Senha.size()<6 || Senha.size()>12) return false;

  // Testa se jah existe usuario com mesmo login
  auto itr = find(LU.begin(), LU.end(), Login);
  if (itr != LU.end()) return false;

  // Insere
  LU.push_back( User(Login,Senha,Admin) );

  // Insercao OK
  return true;
}

/// Remover um usuario
bool SupServidor::removeUser(const string& Login)
{
  // Nao pode ser chamada com servidor em funcionamento
  if (serverOn())
  {
    cerr << "Nao pode remover usuarios com servidor em funcionamento!\n";
    return false;
  }

  // Testa se existe usuario com esse login
  auto itr = find(LU.begin(), LU.end(), Login);
  if (itr == LU.end()) return false;

  // Remove
  LU.erase(itr);

  // Remocao OK
  return true;
}

/// A thread que implementa o servidor.
/// Comunicacao com os clientes atraves dos sockets.
void SupServidor::thr_server_main(void)
{
  // Fila de sockets para aguardar chegada de dados
  mysocket_queue fila;

  while (server_on)
  {
    // Erros mais graves que encerram o servidor
    // Parametro do throw e do catch eh uma const char* = "texto"
    try
    {
      // Encerra se o socket de conexoes estiver fechado
      if (sock_server.closed())
      {
        throw "socket de conexoes fechado";
      }

      // Inclui na fila de sockets todos os sockets que eu
      // quero monitorar para ver se houve chegada de dados

      // Limpa a fila de sockets
      fila.clear();
      // Inclui na fila o socket de conexoes
      if (fila.include(sock_server) != mysocket_status::SOCK_OK)
      {
        throw "erro ao incluir socket de conexoes na fila";
      }
      // Inclui na fila todos os sockets dos clientes conectados
      for (auto& U : LU)
      {
        if (U.isConnected())
        {
          if (fila.include(U.sock) != mysocket_status::SOCK_OK)
          {
            throw "erro ao incluir socket de cliente na fila";
          }
        }
      }

      // Espera ateh que chegue dado em algum socket (com timeout)
      mysocket_status status = fila.wait_read(1000);

      // De acordo com o resultado da espera:
      // SOCK_TIMEOUT:
      // Saiu por timeout: nao houve atividade em nenhum socket
      // Aproveita para salvar dados ou entao nao faz nada
      if (status == mysocket_status::SOCK_TIMEOUT)
      {
        continue;
      }
      // SOCK_ERROR:
      // Erro no select: encerra o servidor
      if (status == mysocket_status::SOCK_ERROR)
      {
        if (!server_on) break;
        throw "erro no select";
      }
      // SOCK_OK:
      // Houve atividade em algum socket da fila:
      //   Testa se houve atividade nos sockets dos clientes. Se sim:
      //   - Leh o comando
      //   - Executa a acao
      //   = Envia resposta
      //   Depois, testa se houve atividade no socket de conexao. Se sim:
      //   - Estabelece nova conexao em socket temporario
      //   - Leh comando, login e senha
      //   - Testa usuario
      //   - Se deu tudo certo, faz o socket temporario ser o novo socket
      //     do cliente e envia confirmacao
      for (auto& U : LU)
      {
        if (!U.isConnected()) continue;
        if (!fila.had_activity(U.sock)) continue;

        uint16_t cmd;
        mysocket_status st_read = U.sock.read_uint16(cmd, SUP_TIMEOUT*1000);

        if (st_read != mysocket_status::SOCK_OK)
        {
          cout << "CMD_LOGOUT " << U.login << endl;
          U.close();
          continue;
        }

        if (cmd == CMD_GET_DATA)
        {
          SupState S;
          readStateFromSensors(S);

          if (U.sock.write_uint16(CMD_DATA) != mysocket_status::SOCK_OK ||
              U.sock.write_bytes(reinterpret_cast<const mybyte*>(&S),
                                 sizeof(S)) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
          }
        }
        else if (cmd == CMD_SET_V1)
        {
          uint16_t estado;

          if (U.sock.read_uint16(estado, SUP_TIMEOUT*1000) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
            continue;
          }

          bool ok = U.isAdmin;
          if (ok) setV1Open(estado != 0);

          if (U.sock.write_uint16(ok ? CMD_OK : CMD_ERROR) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
            continue;
          }

          cout << "CMD_SET_V1 " << estado << " DE " << U.login
               << (ok ? " (OK)" : " (ERROR)") << endl;
        }
        else if (cmd == CMD_SET_V2)
        {
          uint16_t estado;

          if (U.sock.read_uint16(estado, SUP_TIMEOUT*1000) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
            continue;
          }

          bool ok = U.isAdmin;
          if (ok) setV2Open(estado != 0);

          if (U.sock.write_uint16(ok ? CMD_OK : CMD_ERROR) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
            continue;
          }

          cout << "CMD_SET_V2 " << estado << " DE " << U.login
               << (ok ? " (OK)" : " (ERROR)") << endl;
        }
        else if (cmd == CMD_SET_PUMP)
        {
          uint16_t entrada;

          if (U.sock.read_uint16(entrada, SUP_TIMEOUT*1000) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
            continue;
          }

          bool ok = U.isAdmin;
          if (ok) setPumpInput(entrada);

          if (U.sock.write_uint16(ok ? CMD_OK : CMD_ERROR) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
            continue;
          }

          cout << "CMD_SET_PUMP " << entrada << " DE " << U.login
               << (ok ? " (OK)" : " (ERROR)") << endl;
        }
        else if (cmd == CMD_LOGOUT)
        {
          cout << "CMD_LOGOUT " << U.login << endl;
          U.close();
        }
        else
        {
          if (U.sock.write_uint16(CMD_ERROR) != mysocket_status::SOCK_OK)
          {
            cout << "CMD_LOGOUT " << U.login << endl;
            U.close();
          }
        }
      }

      // Testa se houve pedido de nova conexao
      if (server_on && sock_server.accepting() && fila.had_activity(sock_server))
      {
        tcp_mysocket sock_temp;
        string Login, Senha;
        uint16_t cmd;

        bool erro_login = false;

        if (sock_server.accept(sock_temp) != mysocket_status::SOCK_OK)
        {
          throw "erro ao aceitar nova conexao";
        }

        if (sock_temp.read_uint16(cmd, SUP_TIMEOUT*1000) != mysocket_status::SOCK_OK)
        {
          erro_login = true;
        }
        else if (cmd != CMD_LOGIN)
        {
          erro_login = true;
        }
        else if (sock_temp.read_string(Login, SUP_TIMEOUT*1000) != mysocket_status::SOCK_OK)
        {
          erro_login = true;
        }
        else if (sock_temp.read_string(Senha, SUP_TIMEOUT*1000) != mysocket_status::SOCK_OK)
        {
          erro_login = true;
        }

        if (erro_login)
        {
          sock_temp.write_uint16(CMD_ERROR);
          cout << "CMD_LOGIN " << Login << " (ERROR)" << endl;
          sock_temp.close();
        }
        else
        {
          auto itr = find(LU.begin(), LU.end(), Login);

          if (itr != LU.end() &&
              itr->password == Senha &&
              !itr->isConnected())
          {
            itr->sock.swap(sock_temp);

            uint16_t resposta = (itr->isAdmin ? CMD_ADMIN_OK : CMD_OK);

            if (itr->sock.write_uint16(resposta) == mysocket_status::SOCK_OK)
            {
              cout << "CMD_LOGIN " << Login << " (OK)" << endl;
            }
            else
            {
              cout << "CMD_LOGIN " << Login << " (ERROR)" << endl;
              itr->close();
            }
          }
          else
          {
            sock_temp.write_uint16(CMD_ERROR);
            cout << "CMD_LOGIN " << Login << " (ERROR)" << endl;
            sock_temp.close();
          }
        }
      }
    } // fim try - Erros mais graves que encerram o servidor
    catch(const char* err)  // Erros mais graves que encerram o servidor
    {
      cerr << "Erro no servidor: " << err << endl;

      // Sai do while e encerra a thread
      server_on = false;

      // Fecha todos os sockets dos clientes
      for (auto& U : LU) U.close();
      // Fecha o socket de conexoes
      sock_server.close();

      // Os tanques continuam funcionando

    } // fim catch - Erros mais graves que encerram o servidor
  } // fim while (server_on)
}



