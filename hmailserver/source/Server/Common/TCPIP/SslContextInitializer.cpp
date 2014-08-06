// Copyright (c) 2010 Martin Knafve / hMailServer.com.  
// http://www.hmailserver.com

#include "StdAfx.h"
#include "SslContextInitializer.h"

#include "../BO/SSLCertificate.h"
#include "../Util/Encoding/Base64.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int CertificateAlreadyInStore = 185057381;

   bool
   SslContextInitializer::InitServer(boost::asio::ssl::context& context, shared_ptr<SSLCertificate> certificate, String ip_address, int port)
   {  
      if (!certificate)
      {
         String errorMessage = Formatter::Format("Error initializing SSL. Certificate not set. Address: {0}, Port: {1}", ip_address, port);
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5113, "SslContextInitializer::InitServer", errorMessage);
         return false;
      }

      try
      {
         context.set_options(boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2);
      }
      catch (boost::system::system_error ec)
      {
         String asioError = ec.what();

         String errorMessage;
         errorMessage.Format(_T("Failed to set SSL context options. Address: %s, Port: %i, Error: %s"), 
            String(ip_address), port, asioError);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5113, "SslContextInitializer::InitServer", errorMessage);

         return false;

      }

      AnsiString certificateFile = certificate->GetCertificateFile();
      AnsiString privateKeyFile = certificate->GetPrivateKeyFile();


      try
      {
         context.use_certificate_file(certificateFile, boost::asio::ssl::context::pem);
      }
      catch (boost::system::system_error ec)
      {
         String asioError = ec.what();

         String errorMessage;
         errorMessage.Format(_T("Failed to load certificate file. Path: %s, Address: %s, Port: %i, Error: %s"), 
            String(certificateFile), ip_address, port, asioError);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5113, "SslContextInitializer::InitServer", errorMessage);

         return false;
      }

      try
      {
         context.use_certificate_chain_file(certificateFile);
      }
      catch (boost::system::system_error ec)
      {
         String asioError = ec.what();

         String errorMessage;
         errorMessage.Format(_T("Failed to load certificate chain from certificate file. Path: %s, Address: %s, Port: %i, Error: %s"), 
            String(certificateFile), ip_address, port, asioError);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5113, "SslContextInitializer::InitServer", errorMessage);

         return false;
      }

      try
      {
         context.set_password_callback(boost::bind(&SslContextInitializer::GetPassword_));
         context.use_private_key_file(privateKeyFile, boost::asio::ssl::context::pem);
      }
      catch (boost::system::system_error ec)
      {
         String asioError = ec.what();

         String errorMessage;
         errorMessage.Format(_T("Failed to load private key file. Path: %s, Address: %s, Port: %i, Error: %s"), 
            String(privateKeyFile), ip_address, port, asioError);

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5113, "SslContextInitializer::InitServer", errorMessage);

         return false;
      }
      catch (...)
      {
         String errorMessage = "Error initializing SSL";
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5113, "SslContextInitializer::InitServer", errorMessage);
         return false;
      }

      SetCipherList_(context);

      return true;
   }

   bool
   SslContextInitializer::InitClient(boost::asio::ssl::context& context)
   { 
      boost::system::error_code errorCode;
      context.set_options(boost::asio::ssl::context::default_workarounds |
         boost::asio::ssl::context::no_sslv2);

      if (errorCode.value() != 0)
      {
         String errorMessage;
         errorMessage.Format(_T("Failed to set default workarounds."));

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5144, "SslContextInitializer::InitClient", errorMessage, errorCode);

         return false;
      }

      if (Configuration::Instance()->GetVerifyRemoteSslCertificate())
      {
         context.set_verify_mode(boost::asio::ssl::context::verify_peer | boost::asio::ssl::context::verify_fail_if_no_peer_cert, errorCode);
         
         if (errorCode.value() != 0)
         {
            String errorMessage;
            errorMessage.Format(_T("Failed to enable peer verification."));
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5144, "SslContextInitializer::InitClient", errorMessage, errorCode);

            return false;
         }

         if (IniFileSettings::Instance()->GetManulCAFilesExists())
         {
            context.add_verify_path(AnsiString(IniFileSettings::Instance()->GetCertificateAuthorityDirectory()), errorCode);
            if (errorCode.value() != 0)
            {
               String errorMessage;
               errorMessage.Format(_T("Failed to add path to Certificate Authority files."));
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5144, "TCPServer::InitSSL", errorMessage, errorCode);
               return false;
            }
         }
         else
         {
            if (!LoadWindowsCA_(context))
            {
               return false;
            }
         }
      }
      else
      {
         context.set_verify_mode(boost::asio::ssl::context::verify_none);
      }

      SetCipherList_(context);

      return true;
   }

   bool
   SslContextInitializer::LoadWindowsCA_(boost::asio::ssl::context& context)
   {
      LOG_DEBUG("Loading certificates from Windows Certificate Root store...");

      HCERTSTORE store = CertOpenSystemStore(NULL, _T("ROOT"));

      if (store == NULL)
      {
         String errorMessage = Formatter::Format(_T("Failed to open Windows certificate store. GetLastError() == {0}"), (int) GetLastError());
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5504, "SslContextInitializer::LoadWindowsCA_", errorMessage);
         return false;
      }

      int certificate_count = 0;

      for ( PCCERT_CONTEXT pCertCtx = CertEnumCertificatesInStore(store, NULL);
                  pCertCtx != NULL;
                  pCertCtx = CertEnumCertificatesInStore(store, pCertCtx) )
      {
         AddCertificate_(context, false, pCertCtx->dwCertEncodingType, pCertCtx->pbCertEncoded, pCertCtx->cbCertEncoded);

         certificate_count++;
      }

      for ( PCCRL_CONTEXT pCrlCtx = CertEnumCRLsInStore(store, NULL);
                  pCrlCtx != NULL;
                  pCrlCtx = CertEnumCRLsInStore(store, pCrlCtx) )
      {
         AddCertificate_(context, true, pCrlCtx->dwCertEncodingType, pCrlCtx->pbCrlEncoded, pCrlCtx->cbCrlEncoded);

         certificate_count++;
      }

      LOG_DEBUG(Formatter::Format("{0} certificates were loaded from the Windows Certificate Root store.", certificate_count));

      CertCloseStore(store, 0);

      return true;
   }

   void
   SslContextInitializer::AddCertificate_(boost::asio::ssl::context& context, bool is_revocation, DWORD encoding_type, BYTE *certificate, int certificate_length)
   {
      AnsiString certificate_type;

      if ((encoding_type & PKCS_7_ASN_ENCODING) == PKCS_7_ASN_ENCODING)
         certificate_type = "PKCS7";
      else if (is_revocation)
         certificate_type = "X509 CRL";
      else
         certificate_type = "CERTIFICATE";

      AnsiString base64Encoded  = Base64::Encode((const char*) certificate, certificate_length);

      AnsiString certData;
      certData.AppendFormat("-----BEGIN %s-----\r\n", certificate_type.GetBuffer());

      int line_length = 63;
      int line_count = (int) ceil((double) base64Encoded.GetLength() / (double) line_length);
      for (int line_index = 0; line_index < line_count; line_index++)
      {
         int start = line_index * line_length;

         AnsiString line = base64Encoded.Mid(start, line_length);
         certData.AppendFormat("%s\r\n", line.GetBuffer());
      }

      certData.AppendFormat("-----END %s-----\r\n", certificate_type.GetBuffer());

      boost::asio::const_buffer buf(certData.GetBuffer(-1), certData.GetLength());

      boost::system::error_code ec;
      context.add_certificate_authority(buf, ec);

      int error = ec.value();
      if (error > 0 && error != CertificateAlreadyInStore)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5505, "SslContextInitializer::AddCertificate_", "Failed to add CA certificate.", ec);
      }
      
   }
   std::string 
   SslContextInitializer::GetPassword_()
   {
      ErrorManager::Instance()->ReportError(ErrorManager::High, 5143, "TCPServer::GetPassword()", "The private key file has a password. hMailServer does not support this.");
      return "";
   }


   void 
   SslContextInitializer::SetCipherList_(boost::asio::ssl::context& context)
   {
      AnsiString cipher_list = Configuration::Instance()->GetSslCipherList();

      cipher_list.Replace("\r", "");
      cipher_list.Replace("\n", "");
      cipher_list.Replace(" ", "");

      if (cipher_list.Trim().IsEmpty())
         return;

      // Asio does not expose cipher list. Access underlaying layer (OpenSSL) directly.
      SSL_CTX* ssl = context.native_handle();
      int result = SSL_CTX_set_cipher_list(ssl, cipher_list.c_str());

      if (result == 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5511,"SslContextInitializer::SetCipherList_", "Failed to set SSL ciphers");
     
      }
   }

}